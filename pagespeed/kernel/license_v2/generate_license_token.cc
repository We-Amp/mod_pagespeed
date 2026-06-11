// Copyright (c) 2024-2026 We-Amp B.V.
//
// Generates a signed license token from a 32-byte Ed25519 seed file.
//
// Usage:
//   generate_license_token --key <path> --sub <email>
//       [--exp <unix_timestamp> | --exp-duration <seconds>]
//       [--products <comma-separated>]
//
// Output: base64url-encoded token written to stdout.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/license_v2/license_signer.h"
#include "pagespeed/kernel/license_v2/license_token.h"

namespace {

// the design record: minting paths self-police a 720-day lifetime ceiling. The verifier
// enforces 730 days (kMaxTokenLifetimeSec) and silently rejects beyond it; the
// 10-day margin keeps hand-minted tokens from ever brushing the verifier cap.
constexpr int64_t kMaxMintLifetimeSec = 720LL * 24 * 3600;

void PrintUsage(const char* argv0) {
  std::cerr << "Usage: " << argv0
            << " --key <path> --sub <email>"
               " [--exp <unix_timestamp> | --exp-duration <seconds>]"
               " [--products <comma-separated>]"
               " [--entitlements <comma-separated>]"
               " [--plan <plan>] [--scope <scope> --domain <domain>]\n"
            << "\n"
            << "  --key <path>              REQUIRED. Path to 32-byte Ed25519 "
               "seed file.\n"
            << "  --sub <email>             REQUIRED. Subscriber email.\n"
            << "  --exp <unix_timestamp>    Absolute expiry (mutually "
               "exclusive with --exp-duration).\n"
            << "  --exp-duration <seconds>  Expiry relative to now (mutually "
               "exclusive with --exp).\n"
            << "  --products <list>         Comma-separated product list "
               "(default: mps1,the 2.0 optimizer line).\n"
            << "  --entitlements <list>     Comma-separated feature "
               "entitlements.\n"
            << "  --plan <plan>             Plan label embedded in the token "
               "(default: production).\n"
            << "  --scope <scope>           License scope: one of "
               "community, site, org, host. Default: none (legacy "
               "scopeless token).\n"
            << "  --domain <domain>         Registrable domain the scope "
               "binds to. Required for community/site/host scopes; not "
               "allowed for org.\n"
            << "\n"
            << "  Lifetime is capped at 720 days from now.\n";
}

std::vector<GoogleString> SplitComma(const char* s) {
  std::vector<GoogleString> result;
  std::istringstream stream(s);
  std::string item;
  while (std::getline(stream, item, ',')) {
    if (!item.empty()) {
      result.push_back(item);
    }
  }
  return result;
}

}  // namespace

int main(int argc, char* argv[]) {
  const char* key_path = nullptr;
  const char* sub = nullptr;
  const char* products_str = nullptr;
  const char* entitlements_str = nullptr;
  const char* plan = nullptr;
  const char* scope = nullptr;
  const char* domain = nullptr;
  int64_t exp = 0;
  bool has_exp = false;
  int64_t exp_duration = 0;
  bool has_exp_duration = false;

  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--key") == 0 && i + 1 < argc) {
      key_path = argv[++i];
    } else if (strcmp(argv[i], "--sub") == 0 && i + 1 < argc) {
      sub = argv[++i];
    } else if (strcmp(argv[i], "--exp") == 0 && i + 1 < argc) {
      exp = std::stoll(argv[++i]);
      has_exp = true;
    } else if (strcmp(argv[i], "--exp-duration") == 0 && i + 1 < argc) {
      exp_duration = std::stoll(argv[++i]);
      has_exp_duration = true;
    } else if (strcmp(argv[i], "--products") == 0 && i + 1 < argc) {
      products_str = argv[++i];
    } else if (strcmp(argv[i], "--entitlements") == 0 && i + 1 < argc) {
      entitlements_str = argv[++i];
    } else if (strcmp(argv[i], "--plan") == 0 && i + 1 < argc) {
      plan = argv[++i];
    } else if (strcmp(argv[i], "--scope") == 0 && i + 1 < argc) {
      scope = argv[++i];
    } else if (strcmp(argv[i], "--domain") == 0 && i + 1 < argc) {
      domain = argv[++i];
    } else {
      std::cerr << "Error: unknown flag '" << argv[i] << "'\n\n";
      PrintUsage(argv[0]);
      return 1;
    }
  }

  if (key_path == nullptr) {
    std::cerr << "Error: --key is required.\n\n";
    PrintUsage(argv[0]);
    return 1;
  }
  if (sub == nullptr) {
    std::cerr << "Error: --sub is required.\n\n";
    PrintUsage(argv[0]);
    return 1;
  }
  if (has_exp && has_exp_duration) {
    std::cerr << "Error: --exp and --exp-duration are mutually exclusive.\n\n";
    PrintUsage(argv[0]);
    return 1;
  }

  // the design record scope/domain pairing rules. Scopeless tokens stay valid (R9/R10:
  // legacy tokens classify licensed; scope is never a verify-fail condition).
  if (scope != nullptr) {
    const bool valid_scope =
        strcmp(scope, "community") == 0 || strcmp(scope, "site") == 0 ||
        strcmp(scope, "org") == 0 || strcmp(scope, "host") == 0;
    if (!valid_scope) {
      std::cerr << "Error: --scope must be one of community, site, org, "
                   "host (got '"
                << scope << "').\n\n";
      PrintUsage(argv[0]);
      return 1;
    }
    if (strcmp(scope, "org") == 0) {
      if (domain != nullptr) {
        std::cerr << "Error: --domain is not allowed with --scope org "
                     "(org licenses are organization-wide).\n\n";
        PrintUsage(argv[0]);
        return 1;
      }
    } else if (domain == nullptr || domain[0] == '\0') {
      std::cerr << "Error: --scope " << scope << " requires --domain.\n\n";
      PrintUsage(argv[0]);
      return 1;
    }
  } else if (domain != nullptr) {
    std::cerr << "Error: --domain requires --scope.\n\n";
    PrintUsage(argv[0]);
    return 1;
  }

  // Read the 32-byte seed from the key file.
  std::ifstream key_file(key_path, std::ios::binary);
  if (!key_file) {
    std::cerr << "Error: cannot open key file '" << key_path << "'.\n";
    return 1;
  }
  char seed[32];
  key_file.read(seed, 32);
  if (key_file.gcount() != 32) {
    std::cerr << "Error: key file must be exactly 32 bytes (got "
              << key_file.gcount() << ").\n";
    return 1;
  }
  // Make sure there is no extra data.
  char extra;
  if (key_file.read(&extra, 1)) {
    std::cerr << "Error: key file must be exactly 32 bytes (file is larger).\n";
    return 1;
  }

  GoogleString public_key, private_key;
  net_instaweb::CreateKeypair(StringPiece(seed, 32), &public_key, &private_key);

  int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count();

  net_instaweb::LicensePayload payload;
  payload.sub = sub;
  payload.iss = "modpagespeed.com";
  payload.iat = now;
  payload.plan = (plan != nullptr) ? plan : "production";
  if (scope != nullptr) {
    payload.scope = scope;
  }
  if (domain != nullptr) {
    payload.domain = domain;
  }

  if (products_str != nullptr) {
    payload.products = SplitComma(products_str);
  } else {
    payload.products = {"mps1", "the 2.0 optimizer line"};
  }

  // the design record: entitlements have NO default — absent = empty = no gated features
  // (opt-in safe default, unlike products).
  if (entitlements_str != nullptr) {
    payload.entitlements = SplitComma(entitlements_str);
  }

  if (has_exp) {
    payload.exp = exp;
  } else if (has_exp_duration) {
    payload.exp = now + exp_duration;
  } else {
    // Default: 1 year from now.
    payload.exp = now + static_cast<int64_t>(365 * 24 * 3600);
  }

  // exp == 0 is the verifier's never-expiring escape hatch (it skips both
  // the lifetime cap and expiry when exp is absent/zero). The mint tool is
  // the only police for the design record's 720-day rule, so refuse to mint it.
  if (payload.exp <= 0) {
    std::cerr << "Error: --exp " << payload.exp
              << " would mint a never-expiring token (the verifier skips "
                 "expiry when exp is 0); refusing under the design record "
                 "720-day mint-side cap.\n";
    return 1;
  }

  // the design record mint-side lifetime cap. Hard error, not a clamp: a silently
  // shortened token would surprise whoever hand-minted it.
  if (payload.exp - now > kMaxMintLifetimeSec) {
    std::cerr << "Error: token lifetime exceeds the 720-day mint-side cap "
                 ". Requested "
              << (payload.exp - now) / (24LL * 3600)
              << " days from now; the verifier rejects tokens minted for "
                 "more than 730 days regardless.\n";
    return 1;
  }

  std::cout << net_instaweb::SignLicenseToken(payload, public_key, private_key);
  return 0;
}
