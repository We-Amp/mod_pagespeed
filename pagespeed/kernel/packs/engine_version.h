// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef PAGESPEED_KERNEL_PACKS_ENGINE_VERSION_H_
#define PAGESPEED_KERNEL_PACKS_ENGINE_VERSION_H_

#include <string_view>

namespace pagespeed::packs {

// The version a pack's `engine_min` is compared against: this module's
// version ("MAJOR.MINOR.PATCH"). Kept apart from the loader so the loader
// itself stays free of any product header.
std::string_view EngineVersion();

}  // namespace pagespeed::packs

#endif  // PAGESPEED_KERNEL_PACKS_ENGINE_VERSION_H_
