// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - Config File Persistence
//
// Persists hot-reloadable WorkerConfig fields to a JSON file so that
// console changes (PATCH /v1/config) survive worker restarts.
// Loading priority: defaults -> config file -> env vars -> CLI flags.

#ifndef SRC_WORKER_CONFIG_FILE_H_
#define SRC_WORKER_CONFIG_FILE_H_

#include <string>
#include <string_view>

#include "nlohmann/json.hpp"
#include "src/worker/worker.h"

namespace pagespeed {

// Result of applying JSON fields to a WorkerConfig.
struct ConfigApplyResult {
  nlohmann::json applied;   // Fields successfully applied (key -> new value).
  nlohmann::json rejected;  // Fields rejected (key -> reason string).
  nlohmann::json warnings;  // Warning messages (JSON array of strings).
};

// Returns true if `key` names a non-reloadable config field (e.g. socket_path,
// cache_path, num_threads, ...).
bool IsNonReloadable(const std::string& key);

// Derives the config file path from a cache volume path.
// Returns "{parent_directory}/pagespeed.json".
// Returns empty string if cache_path is empty.
std::string ConfigFilePath(const std::string& cache_path);

// Serializes hot-reloadable WorkerConfig fields to JSON.
nlohmann::json ConfigToJsonPersistable(const WorkerConfig& config);

// True for the 2.0 licensing settings retired at 2.1: license_key,
// license_renewal_url, fastspring_storefront, fastspring_product.
// ApplyConfigJson reports them as rejected with kRetiredLicensingKeyReason
// (never fatal), so a stale pagespeed.json or console still loads.
bool IsRetiredLicensingKey(std::string_view key);
extern const char* const kRetiredLicensingKeyReason;

// quality_cap_margin no longer changes the same-format quality cap.  When a
// non-default value is applied (config file or config PATCH), the result's
// warnings carry this text; use no_quality_cap to turn the cap off.
extern const char* const kQualityCapMarginDeprecatedWarning;

// Validates and applies JSON fields to a WorkerConfig.
// Non-reloadable fields and retired licensing fields are rejected with a
// reason.  Returns applied/rejected/warnings summary.
ConfigApplyResult ApplyConfigJson(const nlohmann::json& body,
                                  WorkerConfig* config);

// Reads and parses a JSON config file. Returns empty object {} on missing
// or corrupt file (never fatal). Sets *error to a description on parse failure.
nlohmann::json ReadConfigFile(const std::string& path, std::string* error);

// Atomically writes JSON to a config file (tmp + fsync + rename, mode 0644).
// Returns true on success.
bool WriteConfigFile(const std::string& path, const nlohmann::json& json);

}  // namespace pagespeed

#endif  // SRC_WORKER_CONFIG_FILE_H_
