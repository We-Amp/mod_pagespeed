// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

// The two registrations of the module image.
//
// IIS calls RegisterModule once for every <globalModules> entry that names
// this DLL.  The module has two entries:
//
//   * the main one (PageSpeedModule by default, any other name the operator
//     gave it): every request notification the module handles, and the
//     process-wide global notification;
//   * the final-view entry, kIisFinalViewModuleName: one send notification at
//     PRIORITY_ALIAS_FIRST, which IIS runs after its own header stages
//     (customHeaders) and dynamic compression, so it sees the response as it
//     goes out.  It reads that view for the HTML cache and changes nothing.
//
// What must hold whichever entry IIS registers first: the process-wide setup
// (the configuration factory, the global notification) runs once, and the
// main registration's module id, which keys every module context the module
// keeps for a request, is set by the main registration and by no other.
// This file holds that decision, free of the IIS headers so it is tested on
// every platform; dll_main.cc acts on it.

#ifndef PAGESPEED_IIS_IIS_REGISTRATION_H_
#define PAGESPEED_IIS_IIS_REGISTRATION_H_

#include <cwchar>
#include <mutex>

namespace net_instaweb {

// The <modules> and <globalModules> name of the final-view entry.  The
// installer adds it at the top of <modules>, so its send runs after every
// other module's at the same priority.
inline constexpr wchar_t kIisFinalViewModuleName[] = L"PageSpeedFinalView";
// The same name, for log lines.
inline constexpr char kIisFinalViewModuleNameAscii[] = "PageSpeedFinalView";
static_assert(sizeof(kIisFinalViewModuleName) / sizeof(wchar_t) ==
                  sizeof(kIisFinalViewModuleNameAscii),
              "the two spellings of the final-view entry's name differ");

enum class IisRegistrationKind {
  kMain,
  kFinalView,
};

// The final-view entry is recognised by its name (compared without regard
// to ASCII case, as IIS compares module names); every other name is the main
// registration, which keeps an operator's own name for it working.
inline IisRegistrationKind IisRegistrationKindForName(const wchar_t* name) {
  if (name == nullptr) return IisRegistrationKind::kMain;
  const wchar_t* want = kIisFinalViewModuleName;
  for (;; ++name, ++want) {
    wchar_t a = *name;
    wchar_t b = *want;
    if (a >= L'A' && a <= L'Z') a = static_cast<wchar_t>(a - L'A' + L'a');
    if (b >= L'A' && b <= L'Z') b = static_cast<wchar_t>(b - L'A' + L'a');
    if (a != b) return IisRegistrationKind::kMain;
    if (a == L'\0') return IisRegistrationKind::kFinalView;
  }
}

// One per process.  Thread-safe (IIS registers on one thread, but nothing
// here relies on that).
class IisRegistrations {
 public:
  // True for exactly one call per process: the caller then runs the
  // process-wide setup.
  bool BeginProcessSetup() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (process_setup_done_) return false;
    process_setup_done_ = true;
    return true;
  }

  // Records a registration's module id.  The main id is taken from the main
  // registration only; a final-view registration, first or later, never
  // replaces it.  Returns false for a second registration of the same kind
  // (a configuration that lists one entry twice), which the caller refuses.
  bool NoteRegistered(IisRegistrationKind kind, const void* module_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const void** slot =
        kind == IisRegistrationKind::kMain ? &main_id_ : &final_view_id_;
    if (*slot != nullptr) return false;
    *slot = module_id;
    return true;
  }

  const void* main_id() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return main_id_;
  }
  const void* final_view_id() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return final_view_id_;
  }

 private:
  mutable std::mutex mutex_;
  bool process_setup_done_ = false;
  const void* main_id_ = nullptr;
  const void* final_view_id_ = nullptr;
};

}  // namespace net_instaweb

#endif  // PAGESPEED_IIS_IIS_REGISTRATION_H_
