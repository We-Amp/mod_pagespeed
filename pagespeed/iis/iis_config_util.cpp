// Shared pagespeed.config resolution.
//
// Single source of truth for the config precedence chain, replacing the two
// hand-copied %ProgramData% probe blocks that used to live in
// iis_module_factory.cpp and iis_misc.cpp (the TODO(oschaaf) "deduplicate this
// code across the code base"). See iis_config_util.h for the documented order.
#define _WINSOCKAPI_
#include <Windows.h>
#include "Shlobj.h"

#include <fstream>
#include <sstream>

#include "pagespeed/iis/iis_config_util.h"
// FindConfigFile() + CONFIGFILE_PRIMARY/FALLBACK live here (global namespace).
#include "pagespeed/iis/iis_configuration.h"

namespace iis_config_util {

namespace {

bool FileExists(const std::string& path) {
  return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// %ProgramData% base directories in canonical-first order (tiers 1/2). The
// trailing backslash lets the CONFIGFILE_* append below join cleanly.
const char* const kProgramDataDirs[] = {
    "\\We-Amp\\PageSpeed\\",    // the design record tier 1: machine-global base default
    "\\We-Amp\\IISWebSpeed\\",  // the design record tier 2: legacy IISpeed fallback (upgrade only)
};

}  // namespace

std::string ResolveProgramDataConfig(bool* exists) {
  if (exists != nullptr) {
    *exists = false;
  }
  CHAR szPath[MAX_PATH];
  if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_COMMON_APPDATA, NULL, 0, szPath))) {
    const std::string pdata(szPath);
    for (const char* dir : kProgramDataDirs) {
      // Within each directory: pagespeed.config before iiswebspeed.config
      // (the legacy upgrade-from-IISpeed inner fallback).
      std::string primary = pdata + dir + CONFIGFILE_PRIMARY;
      if (FileExists(primary)) {
        if (exists != nullptr) {
          *exists = true;
        }
        return primary;
      }
      std::string fallback = pdata + dir + CONFIGFILE_FALLBACK;
      if (FileExists(fallback)) {
        if (exists != nullptr) {
          *exists = true;
        }
        return fallback;
      }
    }
    // Nothing exists yet — return the canonical path so a downstream parser
    // surfaces a "not found" message pointing at where the config _should_ be.
    return pdata + kProgramDataDirs[0] + CONFIGFILE_PRIMARY;
  }
  return std::string();
}

bool SiteConfigExists(const std::string& site_physical_path) {
  if (site_physical_path.empty()) {
    return false;
  }
  // ::FindConfigFile appends CONFIGFILE_PRIMARY/FALLBACK to the site's physical
  // path (which IIS returns with a trailing backslash) and prefers
  // pagespeed.config; it returns the fallback path unconditionally when the
  // primary is absent, so re-check existence here.
  return FileExists(::FindConfigFile(site_physical_path));
}

std::vector<std::string> ResolveConfigPaths(
    const std::string& site_physical_path) {
  std::vector<std::string> out;
  bool base_exists = false;
  std::string base = ResolveProgramDataConfig(&base_exists);
  if (base_exists) {
    out.push_back(base);  // tiers 1/2 (base layer)
  }
  if (SiteConfigExists(site_physical_path)) {
    out.push_back(::FindConfigFile(site_physical_path));  // tier 3 (override)
  }
  return out;
}

std::string ResolveEffectiveConfigFile(const std::string& site_physical_path) {
  std::vector<std::string> chain = ResolveConfigPaths(site_physical_path);
  return chain.empty() ? std::string() : chain.back();
}

bool ConfigContentsEqual(const std::string& a, const std::string& b) {
  std::ifstream fa(a.c_str(), std::ios::binary);
  std::ifstream fb(b.c_str(), std::ios::binary);
  if (!fa.good() || !fb.good()) {
    // Two unreadable files are "equal" (nothing to warn about); a readable and
    // an unreadable file are unequal.
    return fa.good() == fb.good();
  }
  std::stringstream sa, sb;
  sa << fa.rdbuf();
  sb << fb.rdbuf();
  return sa.str() == sb.str();
}

}  // namespace iis_config_util
