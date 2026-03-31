// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef PAGESPEED_SYSTEM_JSON_UTILS_H_
#define PAGESPEED_SYSTEM_JSON_UTILS_H_

#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"

namespace net_instaweb {

// NOTE: JsonEscape() now lives in pagespeed/kernel/base/string_util.h
// (inline, canonical source from pagespeed-optimizer).

// Extract a JSON string field from a simple JSON object.
// This is a minimal parser sufficient for our license-service responses;
// it does not handle nested objects, arrays, or Unicode escape sequences.
bool ExtractJsonStringField(StringPiece json, StringPiece field_name,
                            GoogleString* value);

// Extract a JSON boolean field ("true" / "false") from a simple JSON object.
bool ExtractJsonBoolField(StringPiece json, StringPiece field_name,
                          bool* value);

}  // namespace net_instaweb

#endif  // PAGESPEED_SYSTEM_JSON_UTILS_H_
