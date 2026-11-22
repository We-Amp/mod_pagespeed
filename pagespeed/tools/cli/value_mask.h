// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef PAGESPEED_TOOLS_CLI_VALUE_MASK_H_
#define PAGESPEED_TOOLS_CLI_VALUE_MASK_H_

#include <set>
#include <vector>

#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"

namespace net_instaweb {
namespace lint {

// What the linter prints in place of a value it may not show.
inline constexpr char kMasked[] = "<masked>";

// Decides which configuration text the linter may echo.  Option values are
// shown only for the options in the admin console's SAFE_OPTION_NAMES whose
// name does not match its SECRET_NAME pattern (both read from the console's
// sources at build time); every other value is replaced by kMasked.  Names
// that are not a known directive are masked too, as a secret can be glued
// onto a mistyped name.  ShowAll() lifts the mask (--show-values).
class ValueMask {
 public:
  ValueMask();

  void ShowAll() { show_all_ = true; }
  bool showing_all() const { return show_all_; }

  // True when the values of `directive` (as written, e.g.
  // "ModPagespeedRewriteLevel") may be shown.
  bool ValuesAreSafe(StringPiece directive) const;

  // The arguments of `directive` as they may be printed.
  std::vector<GoogleString> Args(StringPiece directive,
                                 const std::vector<GoogleString>& args) const;

  // `text` (a name or a value that is not known to be safe), or kMasked.
  GoogleString Text(StringPiece text) const;

  // The number of option names whose values may be shown, and the counts the
  // generator read from safe-option-names.ts (for the allowlist test).
  int safe_name_count() const { return static_cast<int>(safe_names_.size()); }
  static int SourceEntryCount();
  static int SourceSecretExcludedCount();

 private:
  struct CaseLess {
    bool operator()(const GoogleString& a, const GoogleString& b) const {
      return StringCaseCompare(a, b) < 0;
    }
  };
  std::set<GoogleString, CaseLess> safe_names_;
  bool show_all_ = false;
};

}  // namespace lint
}  // namespace net_instaweb

#endif  // PAGESPEED_TOOLS_CLI_VALUE_MASK_H_
