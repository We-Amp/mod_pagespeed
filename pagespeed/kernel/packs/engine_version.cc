// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "pagespeed/kernel/packs/engine_version.h"

#include "net/instaweb/public/version.h"

namespace pagespeed::packs {

std::string_view EngineVersion() { return MOD_PAGESPEED_VERSION_STRING; }

}  // namespace pagespeed::packs
