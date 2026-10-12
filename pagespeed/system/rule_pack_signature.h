// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef PAGESPEED_SYSTEM_RULE_PACK_SIGNATURE_H_
#define PAGESPEED_SYSTEM_RULE_PACK_SIGNATURE_H_

#include <string>
#include <string_view>

#include "pagespeed/kernel/packs/signature_format.h"

namespace net_instaweb {

// What was found next to a pack file: `<pack path>.sig`.
struct SidecarState {
  bool present = false;  // the file exists
  bool read_ok = false;  // it was read, within the size limit
  std::string text;      // its content when read_ok
};

// Ed25519 verification with BoringSSL; false on any size mismatch.
bool VerifyEd25519(std::string_view message, std::string_view signature,
                   std::string_view public_key);

// Checks a pack's signature in this order, the first failing step naming the
// reason: unsigned, signature unreadable, publisher mismatch, unknown key,
// bad signature. `key_id` (optional) is set when the result is kVerified.
pagespeed::packs::SignatureStatus CheckPackSignature(
    std::string_view pack_bytes, std::string_view publisher,
    const SidecarState& sidecar, const pagespeed::packs::PackTrust& trust,
    std::string* key_id);

}  // namespace net_instaweb

#endif  // PAGESPEED_SYSTEM_RULE_PACK_SIGNATURE_H_
