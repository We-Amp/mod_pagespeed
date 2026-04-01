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

void PrintUsage(const char* argv0) {
  std::cerr << "Usage: " << argv0
            << " --key <path> --sub <email>"
               " [--exp <unix_timestamp> | --exp-duration <seconds>]"
               " [--products <comma-separated>]\n"
            << "\n"
            << "  --key <path>              REQUIRED. Path to 32-byte Ed25519 "
               "seed file.\n"
            << "  --sub <email>             REQUIRED. Subscriber email.\n"
            << "  --exp <unix_timestamp>    Absolute expiry (mutually "
               "exclusive with --exp-duration).\n"
            << "  --exp-duration <seconds>  Expiry relative to now (mutually "
               "exclusive with --exp).\n"
            << "  --products <list>         Comma-separated product list "
               "(default: mps1,the 2.0 optimizer line).\n";
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
  payload.plan = "production";

  if (products_str != nullptr) {
    payload.products = SplitComma(products_str);
  } else {
    payload.products = {"mps1", "the 2.0 optimizer line"};
  }

  if (has_exp) {
    payload.exp = exp;
  } else if (has_exp_duration) {
    payload.exp = now + exp_duration;
  } else {
    // Default: 1 year from now.
    payload.exp = now + static_cast<int64_t>(365 * 24 * 3600);
  }

  std::cout << net_instaweb::SignLicenseToken(payload, public_key, private_key);
  return 0;
}
