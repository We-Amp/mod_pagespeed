// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "pagespeed/tools/cli/apache_reader.h"

#include <openssl/sha.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>

namespace net_instaweb {
namespace lint {

namespace fs = std::filesystem;

namespace {

// Larger files are refused rather than read: a configuration is small, and
// the linter may be run on untrusted input.
constexpr uintmax_t kMaxFileBytes = 16 << 20;
constexpr int kMaxIncludeDepth = 32;

// The containers that put a directive in a directory context.
const char* const kDirectorySections[] = {
    "directory", "directorymatch", "location",  "locationmatch",
    "files",     "filesmatch",     "if",        "elseif",
    "else",      "proxy",          "proxymatch"};

// Container names that are shown as written in a finding; any other name is
// masked, as it may be a mistyped line that carries a secret.
const char* const kKnownSections[] = {
    "virtualhost",   "directory",   "directorymatch", "location",
    "locationmatch", "files",       "filesmatch",     "if",
    "elseif",        "else",        "proxy",          "proxymatch",
    "limit",         "limitexcept", "ifmodule",       "ifdefine",
    "ifversion",     "iffile",      "ifdirective",    "ifsection",
    "requireall",    "requireany",  "requirenone",    "macro",
    "modpagespeedif"};

bool IsOneOf(const GoogleString& lower, const char* const* names, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    if (lower == names[i]) {
      return true;
    }
  }
  return false;
}

// UTF-8 bytes <-> path, the same on every platform (no locale code page).
GoogleString U8(const fs::path& p) {
  std::u8string s = p.generic_u8string();
  return GoogleString(s.begin(), s.end());
}
fs::path FromU8(const GoogleString& s) {
  return fs::path(std::u8string(s.begin(), s.end()));
}

GoogleString HexSha256(const GoogleString& data) {
  unsigned char digest[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const unsigned char*>(data.data()), data.size(),
         digest);
  static const char kHex[] = "0123456789abcdef";
  GoogleString out;
  for (unsigned char c : digest) {
    out.push_back(kHex[c >> 4]);
    out.push_back(kHex[c & 15]);
  }
  return out;
}

GoogleString Lower(StringPiece s) {
  GoogleString out = s.as_string();
  LowerString(&out);
  return out;
}

GoogleString JoinLogical(const std::vector<GoogleString>& components,
                         size_t n) {
  GoogleString out;
  for (size_t i = 0; i < n; ++i) {
    if (i > 0) {
      out += "/";
    }
    out += components[i];
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

// One pattern element of WildMatch at p[*pi] ('?', a '[...]' class or a
// literal) against c.  Advances *pi past the element.
bool MatchOne(StringPiece p, size_t* pi, char c) {
  const size_t i = *pi;
  if (p[i] == '?') {
    *pi = i + 1;
    return true;
  }
  if (p[i] == '[') {
    size_t q = i + 1;
    const bool negate = q < p.size() && (p[q] == '!' || p[q] == '^');
    if (negate) {
      ++q;
    }
    bool hit = false;
    for (bool first = true; q < p.size() && (first || p[q] != ']');
         ++q, first = false) {
      if (q + 2 < p.size() && p[q + 1] == '-' && p[q + 2] != ']') {
        if (c >= p[q] && c <= p[q + 2]) {
          hit = true;
        }
        q += 2;
      } else if (p[q] == c) {
        hit = true;
      }
    }
    if (q < p.size()) {  // at the closing ']'
      *pi = q + 1;
      return hit != negate;
    }
    // No closing ']': the '[' is a literal.
  }
  *pi = i + 1;
  return p[i] == c;
}

// fnmatch() without flags, for one path component: '*', '?' and '[...]',
// case-sensitive.  Greedy with one backtrack point (the last '*'), so the
// time is bounded by the product of the two lengths whatever the pattern.
bool WildMatch(StringPiece p, StringPiece s) {
  size_t pi = 0;
  size_t si = 0;
  size_t star = StringPiece::npos;
  size_t mark = 0;
  while (si < s.size()) {
    if (pi < p.size() && p[pi] == '*') {
      star = pi++;
      mark = si;
      continue;
    }
    size_t next = pi;
    if (pi < p.size() && MatchOne(p, &next, s[si])) {
      pi = next;
      ++si;
      continue;
    }
    if (star == StringPiece::npos) {
      return false;
    }
    pi = star + 1;
    si = ++mark;
  }
  while (pi < p.size() && p[pi] == '*') {
    ++pi;
  }
  return pi == p.size();
}

bool GetWord(StringPiece line, size_t* pos, GoogleString* word) {
  size_t i = *pos;
  while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
    ++i;
  }
  if (i >= line.size()) {
    *pos = i;
    return false;
  }
  word->clear();
  const char quote = line[i];
  if (quote == '"' || quote == '\'') {
    ++i;
    while (i < line.size() && line[i] != quote) {
      if (line[i] == '\\' && i + 1 < line.size() && line[i + 1] == quote) {
        ++i;
      }
      word->push_back(line[i]);
      ++i;
    }
    if (i < line.size()) {
      ++i;
    }
  } else {
    while (i < line.size() && line[i] != ' ' && line[i] != '\t') {
      word->push_back(line[i]);
      ++i;
    }
  }
  *pos = i;
  return true;
}

}  // namespace

GoogleString ScopeLabel(const Placement& placement) {
  GoogleString s;
  switch (placement.kind) {
    case ContextKind::kServer:
      s = "server";
      break;
    case ContextKind::kVirtualHost:
      s = "virtualhost";
      break;
    case ContextKind::kDirectory:
      s = placement.in_virtual_host ? "virtualhost/directory" : "directory";
      break;
    case ContextKind::kHtaccess:
      s = "htaccess";
      break;
  }
  if (placement.in_conditional) {
    s += "+conditional";
  }
  return s;
}

std::vector<GoogleString> ApacheConfigReader::Words(StringPiece line) {
  std::vector<GoogleString> out;
  size_t pos = 0;
  GoogleString w;
  while (GetWord(line, &pos, &w)) {
    out.push_back(w);
  }
  return out;
}

ApacheConfigReader::ApacheConfigReader(const fs::path& root,
                                       const ValueMask* mask, Report* report,
                                       ApacheLineVisitor* visitor)
    : root_(root), mask_(mask), report_(report), visitor_(visitor) {
  std::error_code ec;
  root_resolved_ = fs::canonical(root, ec);
  if (ec) {
    root_resolved_ = root;
  }
}

bool ApacheConfigReader::ReadConfig(StringPiece path) {
  return ReadTop(path, false);
}

bool ApacheConfigReader::ReadHtaccess(StringPiece path) {
  return ReadTop(path, true);
}

bool ApacheConfigReader::ReadTop(StringPiece path, bool htaccess) {
  std::vector<GoogleString> components;
  fs::path disk;
  bool case_mismatch = false;
  if (!Normalise(path, &components) || components.empty() ||
      !ResolveFixed(components, &disk, &case_mismatch)) {
    return false;
  }
  std::error_code ec;
  GoogleString resolved;
  if (!fs::is_regular_file(disk, ec) || !InsideRoot(disk, &resolved)) {
    return false;
  }
  stack_.clear();
  ReadFile(JoinLogical(components, components.size()), disk,
           htaccess ? "htaccess" : "config", htaccess, 0);
  return true;
}

void ApacheConfigReader::AddFinding(const char* rule, Severity severity,
                                    const GoogleString& file, int line,
                                    const Placement& placement,
                                    const GoogleString& directive,
                                    const GoogleString& message) {
  Finding f;
  f.rule = rule;
  f.severity = severity;
  f.file = file;
  f.line = line;
  f.scope = ScopeLabel(placement);
  f.directive = directive;
  f.message = message;
  report_->Add(std::move(f));
}

bool ApacheConfigReader::Normalise(
    StringPiece raw, std::vector<GoogleString>* components) const {
  GoogleString s = raw.as_string();
  std::replace(s.begin(), s.end(), '\\', '/');
  if (s.size() >= 2 && s[1] == ':' &&
      isalpha(static_cast<unsigned char>(s[0]))) {
    s = s.substr(2);  // A drive letter is re-rooted like a leading '/'.
  }
  components->clear();
  StringPieceVector parts;
  SplitStringPieceToVector(s, "/", &parts, true);
  for (StringPiece p : parts) {
    if (p == ".") {
      continue;
    }
    if (p == "..") {
      if (components->empty()) {
        return false;  // Leaves the root.
      }
      components->pop_back();
      continue;
    }
    components->push_back(p.as_string());
  }
  return true;
}

bool ApacheConfigReader::ResolveFixed(
    const std::vector<GoogleString>& components, fs::path* disk,
    bool* case_mismatch) const {
  fs::path cur = root_;
  for (const GoogleString& want : components) {
    std::vector<GoogleString> entries = SortedEntries(cur);
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
    if (count != 1) {
      return false;
    }
    *case_mismatch = true;
    cur /= FromU8(*match);
  }
  *disk = cur;
  return true;
}

bool ApacheConfigReader::InsideRoot(const fs::path& disk,
                                    GoogleString* resolved) const {
  std::error_code ec;
  const fs::path real = fs::canonical(disk, ec);
  if (ec) {
    return false;
  }
  *resolved = U8(real);
  auto r = root_resolved_.begin();
  auto d = real.begin();
  for (; r != root_resolved_.end(); ++r, ++d) {
    if (r->empty()) {
      continue;  // a trailing separator of the root
    }
    if (d == real.end() || *r != *d) {
      return false;
    }
  }
  return true;
}

// Expands components[index..] below `disk`: fixed components resolve as in
// ResolveFixed, a wildcard component matches every sorted entry.  A final
// directory expands to every file below it, sorted and depth first, as httpd
// reads it.  Every path is resolved before it is used: one outside the root
// is skipped, a directory already read whole is skipped (a symbolic link loop),
// and the expansion stops at kMaxIncludeFiles files or kMaxIncludeLevels
// levels.
void ApacheConfigReader::Expand(const std::vector<GoogleString>& components,
                                size_t index, const fs::path& disk,
                                const GoogleString& logical, int levels,
                                ExpandState* state,
                                std::vector<Resolved>* out) const {
  if (state->truncated) {
    return;
  }
  if (levels > kMaxIncludeLevels ||
      out->size() >= static_cast<size_t>(kMaxIncludeFiles)) {
    state->truncated = true;
    return;
  }
  std::error_code ec;
  GoogleString resolved;
  if (!InsideRoot(disk, &resolved)) {
    if (fs::exists(disk, ec)) {
      state->outside_root = true;
    }
    return;
  }
  const bool is_dir = fs::is_directory(disk, ec);
  if (index == components.size()) {
    if (is_dir) {
      // Only the recursive read of a directory can loop.
      if (!state->directories.insert(resolved).second) {
        state->loop = true;
        return;
      }
      for (const GoogleString& e : SortedEntries(disk)) {
        Expand(components, index, disk / FromU8(e), StrCat(logical, "/", e),
               levels + 1, state, out);
      }
    } else if (fs::is_regular_file(disk, ec)) {
      out->push_back({logical, disk});
    }
    return;
  }
  if (!is_dir) {
    return;
  }
  const GoogleString& want = components[index];
  const GoogleString prefix = logical.empty() ? "" : StrCat(logical, "/");
  std::vector<GoogleString> entries = SortedEntries(disk);
  if (HasWildcard(want)) {
    const bool last = index + 1 == components.size();
    for (const GoogleString& e : entries) {
      if (!WildMatch(want, e)) {
        continue;
      }
      const fs::path p = disk / FromU8(e);
      // httpd matches files with the last component and directories with
      // the others.
      if (last ? !fs::is_regular_file(p, ec) : !fs::is_directory(p, ec)) {
        continue;
      }
      Expand(components, index + 1, p, prefix + e, levels + 1, state, out);
    }
    return;
  }
  if (std::binary_search(entries.begin(), entries.end(), want)) {
    Expand(components, index + 1, disk / FromU8(want), prefix + want,
           levels + 1, state, out);
    return;
  }
  const GoogleString* match = nullptr;
  int count = 0;
  for (const GoogleString& e : entries) {
    if (StringCaseEqual(e, want)) {
      match = &e;
      ++count;
    }
  }
  if (count == 1) {
    state->case_mismatch = true;
    Expand(components, index + 1, disk / FromU8(*match), prefix + want,
           levels + 1, state, out);
  }
}

void ApacheConfigReader::Include(StringPiece arg, bool optional,
                                 const GoogleString& file, int line,
                                 const Placement& placement, int depth) {
  const char* directive = optional ? "IncludeOptional" : "Include";
  std::vector<GoogleString> components;
  if (!Normalise(arg, &components) || components.empty()) {
    AddFinding("PS903", Severity::kError, file, line, placement, directive,
               "Include path leaves the configuration root");
    return;
  }
  const GoogleString logical = JoinLogical(components, components.size());
  ExpandState state;
  std::vector<Resolved> files;
  Expand(components, 0, root_, "", 0, &state, &files);
  if (state.outside_root) {
    AddFinding("PS905", Severity::kError, file, line, placement, directive,
               StrCat("Include target resolves outside the configuration "
                      "root (through a symbolic link) and is not read: ",
                      logical));
  }
  if (state.loop) {
    AddFinding("PS904", Severity::kError, file, line, placement, directive,
               StrCat("Include reaches a directory twice (a symbolic link "
                      "loop); it is read once: ",
                      logical));
  }
  if (state.truncated) {
    AddFinding("PS906", Severity::kError, file, line, placement, directive,
               StrCat("Include expands to more than ", kMaxIncludeFiles,
                      " files or ", kMaxIncludeLevels,
                      " directory levels; the rest is not read: ", logical));
  }
  if (files.empty()) {
    if (state.outside_root) {
      return;
    }
    // IncludeOptional ignores a missing target; so does Include for a
    // wildcard that matches nothing in a directory that exists, but the
    // linter reports that too as it is almost always a mistake.
    if (!optional) {
      AddFinding("PS901", Severity::kError, file, line, placement, directive,
                 StrCat("Include target not found: ", logical));
    }
    return;
  }
  if (state.case_mismatch) {
    AddFinding(
        "PS902", Severity::kWarning, file, line, placement, directive,
        StrCat("Include path differs only by letter case from the "
               "file on disk: ",
               logical, " (works on case-insensitive file systems only)"));
  }
  for (const Resolved& r : files) {
    ReadFile(r.logical, r.disk, "include", false, depth + 1);
  }
}

Placement ApacheConfigReader::PlacementFor(bool htaccess) const {
  Placement p;
  p.kind = htaccess ? ContextKind::kHtaccess : ContextKind::kServer;
  for (const Frame& f : stack_) {
    if (f.name == "virtualhost") {
      p.in_virtual_host = true;
      if (p.kind == ContextKind::kServer) {
        p.kind = ContextKind::kVirtualHost;
      }
    } else if (f.name == "modpagespeedif") {
      p.in_conditional = true;
    } else if (f.name == "limit" || f.name == "limitexcept") {
      p.in_limit = true;
    } else if (!htaccess && IsOneOf(f.name, kDirectorySections,
                                    sizeof(kDirectorySections) /
                                        sizeof(kDirectorySections[0]))) {
      p.kind = ContextKind::kDirectory;
    }
  }
  return p;
}

void ApacheConfigReader::ReadFile(const GoogleString& logical,
                                  const fs::path& disk, const char* kind,
                                  bool htaccess, int depth) {
  const Placement top = PlacementFor(htaccess);
  GoogleString key;
  if (!InsideRoot(disk, &key)) {
    AddFinding("PS905", Severity::kError, logical, 0, top, "",
               "File resolves outside the configuration root and is not read");
    return;
  }
  if (++files_read_ > kMaxInputFiles) {
    if (files_read_ == kMaxInputFiles + 1) {
      AddFinding("PS906", Severity::kError, logical, 0, top, "",
                 StrCat("More than ", kMaxInputFiles,
                        " files to read; the rest is not read"));
    }
    return;
  }
  if (open_files_.count(key) > 0 || depth > kMaxIncludeDepth) {
    AddFinding("PS904", Severity::kError, logical, 0, top, "Include",
               "Include cycle, or includes nested too deeply");
    return;
  }
  std::error_code ec;
  const uintmax_t size = fs::file_size(disk, ec);
  if (ec || size > kMaxFileBytes) {
    AddFinding(
        "PS901", Severity::kError, logical, 0, top, "",
        StrCat("Cannot read ", logical, ec ? "" : " (larger than 16 MiB)"));
    return;
  }
  std::ifstream in(disk, std::ios::binary);
  if (!in) {
    AddFinding("PS901", Severity::kError, logical, 0, top, "",
               StrCat("Cannot read ", logical));
    return;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  const GoogleString data = ss.str();
  report_->AddInput({logical, kind, HexSha256(data)});
  open_files_.insert(key);
  const size_t base_depth = stack_.size();

  std::vector<GoogleString> raw;
  {
    std::istringstream lines(data);
    std::string l;
    while (std::getline(lines, l)) {
      if (!l.empty() && l.back() == '\r') {
        l.pop_back();
      }
      raw.push_back(l);
    }
  }
  for (size_t i = 0; i < raw.size(); ++i) {
    const int lineno = static_cast<int>(i) + 1;
    GoogleString line = raw[i];
    // A backslash at the end of a line continues it on the next one.
    while (!line.empty() && line.back() == '\\' && i + 1 < raw.size()) {
      line.pop_back();
      line += raw[++i];
    }
    StringPiece t(line);
    TrimWhitespace(&t);
    if (t.empty() || t[0] == '#') {
      continue;
    }
    const Placement p = PlacementFor(htaccess);
    if (t.starts_with("</")) {
      StringPiece closer = t.substr(2);
      if (!closer.empty() && closer.back() == '>') {
        closer.remove_suffix(1);
      }
      TrimWhitespace(&closer);
      const GoogleString name = closer.as_string();
      const GoogleString lower = Lower(name);
      if (stack_.size() <= base_depth || stack_.back().name != lower) {
        const bool known =
            IsOneOf(lower, kKnownSections,
                    sizeof(kKnownSections) / sizeof(kKnownSections[0]));
        AddFinding(
            "PS104", Severity::kError, logical, lineno, p, "",
            StrCat("Unexpected </", known ? name : mask_->Text(name), ">"));
        continue;
      }
      stack_.pop_back();
      continue;
    }
    if (t[0] == '<') {
      if (t.back() != '>') {
        AddFinding("PS104", Severity::kError, logical, lineno, p, "",
                   "Section opener without a closing '>'");
        continue;
      }
      std::vector<GoogleString> words = Words(t.substr(1, t.size() - 2));
      if (words.empty()) {
        AddFinding("PS104", Severity::kError, logical, lineno, p, "",
                   "Empty section opener");
        continue;
      }
      const GoogleString name = words[0];
      const GoogleString lower = Lower(name);
      const bool known =
          IsOneOf(lower, kKnownSections,
                  sizeof(kKnownSections) / sizeof(kKnownSections[0]));
      if (lower == "modpagespeedif") {
        std::vector<GoogleString> args(words.begin() + 1, words.end());
        visitor_->Conditional(logical, lineno, p, args);
      } else if (p.in_conditional) {
        AddFinding("PS307", Severity::kError, logical, lineno, p,
                   known ? StrCat("<", name, ">") : mask_->Text(name),
                   "Only mod_pagespeed directives should be inside "
                   "<ModPagespeedIf blocks");
      }
      stack_.push_back({lower, known ? name : mask_->Text(name), lineno});
      continue;
    }
    std::vector<GoogleString> words = Words(t);
    const GoogleString& name = words[0];
    std::vector<GoogleString> args(words.begin() + 1, words.end());
    if (StringCaseEqual(name, "Include") ||
        StringCaseEqual(name, "IncludeOptional")) {
      if (!htaccess) {
        for (const GoogleString& a : args) {
          Include(a, StringCaseEqual(name, "IncludeOptional"), logical, lineno,
                  p, depth);
        }
      }
      continue;
    }
    if (!StringCaseStartsWith(name, "ModPagespeed")) {
      if (p.in_conditional) {
        AddFinding("PS307", Severity::kError, logical, lineno, p,
                   mask_->Text(name),
                   "Only mod_pagespeed directives should be inside "
                   "<ModPagespeedIf blocks");
      }
      continue;  // Other modules' directives are not checked.
    }
    visitor_->Directive(logical, lineno, p, name, args);
  }
  while (stack_.size() > base_depth) {
    AddFinding(
        "PS104", Severity::kError, logical, stack_.back().line,
        PlacementFor(htaccess), "",
        StrCat("<", stack_.back().shown_name, "> not closed in this file"));
    stack_.pop_back();
  }
  open_files_.erase(key);
}

}  // namespace lint
}  // namespace net_instaweb
