// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "pagespeed/apache/apache_directive_table.h"

namespace net_instaweb {
namespace apache_directive {

void ForEachOptionCommand(
    const RewriteOptions& config_template,
    const std::function<void(StringPiece option_name,
                             RewriteOptions::OptionScope scope,
                             const char* help_text, bool deprecated)>& fn) {
  for (const RewriteOptions::OptionBase* option :
       config_template.all_options()) {
    // Skip entries with null documentation -- entries lacking doc
    // are an indication that the option is not available for MPS.
    if (option->help_text() != nullptr) {
      fn(option->option_name(), option->scope(), option->help_text(), false);
    }
  }
  const RewriteOptions::Properties* deprecated_properties =
      RewriteOptions::deprecated_properties();
  for (int i = 0, n = deprecated_properties->size(); i < n; ++i) {
    const RewriteOptions::PropertyBase* dep_prop =
        deprecated_properties->property(i);
    fn(dep_prop->option_name(), dep_prop->scope(), dep_prop->help_text(), true);
  }
}

StringPiece CanonicalDirectiveName(StringPiece directive) {
  if (StringCaseEqual(directive, kModPagespeedImgInlineMaxBytes)) {
    return kModPagespeedImageInlineMaxBytes;
  }
  if (StringCaseEqual(directive, kModPagespeedImgMaxRewritesAtOnce)) {
    return kModPagespeedImageMaxRewritesAtOnce;
  }
  return directive;
}

ProcessScopeUse CheckProcessScopeUse(StringPiece directive, VHostHandling mode,
                                     bool in_virtual_host,
                                     bool in_conditional) {
  ProcessScopeUse result;
  if (mode == VHostHandling::kNotProcessScope) {
    return result;
  }
  if (in_virtual_host) {
    GoogleString vhost_error =
        StrCat("Directive ", directive,
               " used inside a <VirtualHost> but applies globally.");
    if (mode == VHostHandling::kErrorInVHost) {
      result.error = vhost_error;
      return result;
    }
    result.warning =
        StrCat(vhost_error, " Accepting for backwards compatibility. ");
  }
  if (in_conditional) {
    result.error =
        StrCat("Global directive ", directive, " invalid inside conditional.");
  }
  return result;
}

bool ParseConditionalArgument(StringPiece arg, bool* for_spdy) {
  if (StringCaseEqual(arg, "spdy>")) {
    *for_spdy = true;
    return true;
  }
  if (StringCaseEqual(arg, "!spdy>")) {
    *for_spdy = false;
    return true;
  }
  return false;
}

}  // namespace apache_directive
}  // namespace net_instaweb
