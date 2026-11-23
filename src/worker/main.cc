// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// ModPageSpeed 2.0 - Factory Worker Main Entry Point

#include <algorithm>
#include <cctype>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "absl/strings/ascii.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_split.h"
#include "lib/base/json_message_handler.h"
#include "lib/base/message_handler.h"
#include "src/browser/agent_fetcher.h"
#include "src/browser/browser_sandbox.h"
#include "src/product_version/build_commit.h"
#include "src/product_version/version.h"
#include "src/worker/cache_dir.h"
#include "src/worker/config_file.h"
#include "src/worker/cpu_budget.h"
#include "src/worker/http_server.h"
#include "src/worker/shared_config.h"
#include "src/worker/syscall_selftest.h"
#include "src/worker/unsafe_force_async_css.h"
#include "src/worker/windows_service.h"
#include "src/worker/worker.h"

namespace {

pagespeed::Worker* g_worker = nullptr;
volatile sig_atomic_t g_shutdown_requested = 0;

void SignalHandler(int /*signal*/) {
  // Guard against re-entrancy: a second signal during the first
  // handler must not corrupt state.  Only call async-signal-safe
  // functions here.
  if (g_shutdown_requested != 0) return;
  g_shutdown_requested = 1;
  // RequestShutdown() only touches atomics and uv_async_send.
  if (g_worker != nullptr) {
    g_worker->RequestShutdown();
  }
}

// Parse and validate a float CLI argument within [min, max].
// Returns false and prints an error on invalid input, NaN, Inf, or out-of-range.
bool ParseFloat(const char* flag, const char* value, float min, float max,
                float* out) {
  try {
    float v = std::stof(value);
    if (!std::isfinite(v) || v < min || v > max) {
      std::cerr << "Error: " << flag << " must be " << min << "-" << max
                << " (got " << value << ")\n";
      return false;
    }
    *out = v;
    return true;
  } catch (const std::exception&) {
    std::cerr << "Error: " << flag << " invalid value: " << value << "\n";
    return false;
  }
}

// The Windows-only options; they do not exist anywhere else.
#ifdef _WIN32
constexpr const char* kServiceUsage =
    "  --service                  Run under the Service Control Manager\n"
    "  --log-file PATH            With --service: append the log to PATH\n";
#else
constexpr const char* kServiceUsage = "";
#endif

void PrintUsage(const char* program) {
  std::cerr
      << "Usage: " << program << " [options]\n"
      << "Options:\n"
      << "  --socket PATH              Unix socket path "
         "(default: /run/pagespeed-optimizer/notify.sock)\n"
      << "  --cache-dir DIR            Cache directory; the volume, shared "
         "config and\n"
      << "                             serve-stats live inside it (default: "
         "/var/cache/pagespeed-optimizer/v3)\n"
      << "  --cache-path PATH          Expert override: full cache volume "
         "file STEM\n"
      << "                             (deliberately extensionless); wins "
         "over --cache-dir\n"
      << "  --cache-size BYTES         Cache size in bytes "
         "(default: 1073741824)\n"
      << "  --ram-cache-size BYTES     RAM cache size in bytes, "
         "0 to disable (default: 67108864)\n"
      << "  --read-lease-duration MS   Cache read-lease duration for "
         "wrap deferral, 0 to disable leases (default: 5000)\n"
      << "  --lease-wrap-ceiling MS    Max continuous wrap deferral "
         "before a wrap is forced (default: 60000)\n"
      << "  --num-threads N            Thread pool size for "
         "notification processing (default: auto)\n"
      << "  --max-connections N        Max simultaneous connections "
         "(default: 128)\n"
      << "  --max-buffer-size BYTES    Max per-client buffer size "
         "(default: 1048576)\n"
      << "  --connection-timeout MS    Idle connection timeout in ms "
         "(default: 30000)\n"
      << "  --shutdown-timeout MS      Graceful shutdown timeout "
         "in ms (default: 5000)\n"
      << "  --disable-html             Disable HTML optimization\n"
      << "  --disable-css              Disable CSS minification\n"
      << "  --disable-js               Disable JS minification\n"
      << "  --disable-image            Disable image transcoding\n"
      << "  --proactive-image-variants  Enable proactive "
         "multi-format image generation (default: off)\n"
      << "  --no-proactive-viewport-variants  Disable proactive "
         "viewport sibling generation\n"
      << "  --no-proactive-savedata-variants  Disable proactive "
         "Save-Data sibling generation\n"
      << "  --no-proactive-density-variants   Disable proactive "
         "pixel density sibling generation\n"
      << "  --enable-warmup            Enable hot URL variant "
         "warmup (default: off)\n"
      << "  --no-lazy-load-images      Disable loading=\"lazy\" "
         "injection\n"
      << "  --no-image-dimensions      Disable image width/height "
         "injection\n"
      << "  --no-lcp-preload           Disable LCP image preload "
         "injection\n"
      << "  --no-preconnect-injection  Disable preconnect hint "
         "injection\n"
      << "  --no-async-css             Disable async CSS loading "
         "(keep stylesheets render-blocking and inline no above-the-fold "
         "CSS)\n"
      << "  --async-css-min-coverage F Min critical/total CSS coverage to "
         "defer a stylesheet, 0-1 (default: 0.10; 0 disables the gate)\n"
      << "  --async-css-min-deferred-bytes N  Sheets below this size always "
         "defer regardless of coverage (default: 15000)\n"
      << "  --enable-speculation-rules Enable speculation rules "
         "injection (default: off)\n"
      << "  --no-content-analysis      Disable content-aware "
         "quality presets\n"
      << "  --no-preserve-c2pa         Disable C2PA/Content "
         "Credentials provenance preservation (on by default)\n"
      << "  --c2pa-carry               Recompress manifest-bearing PNGs and "
         "re-splice C2PA chunks (PNG only; default: off, serve original)\n"
      << "  --denoise-threshold LEVEL  Deprecated and ignored "
         "(accepted so existing command lines keep working)\n"
      << "  --denoise-sigma-spatial F  Deprecated and ignored\n"
      << "  --denoise-sigma-range F    Deprecated and ignored\n"
      << "  --no-quality-verify        Disable SSIMULACRA2 "
         "quality verification\n"
      << "  --target-ssimulacra2 SCORE Target SSIMULACRA2 score "
         "0-100 (default: 70)\n"
      << "  --ssimulacra2-tolerance F  Acceptable score deviation "
         "(default: 5.0)\n"
      << "  --no-learned-quality       Disable ML quality prediction\n"
      << "  --no-learned-quality-jpeg  Disable ML prediction for JPEG\n"
      << "  --no-learned-quality-webp  Disable ML prediction for WebP\n"
      << "  --no-learned-quality-avif  Disable ML prediction for AVIF\n"
      << "  --savedata-score-reduction F  Target SSIMULACRA2 reduction "
         "for Save-Data (default: 15.0)\n"
      << "  --gzip-level N             Gzip compression level "
         "1-9 (default: 6, 0=disable)\n"
      << "  --brotli-level N           Brotli compression quality "
         "1-11 (default: 6, 0=disable)\n"
      << "  --no-css-import-flattening Disable CSS @import "
         "flattening\n"
      << "  --mobile-width PIXELS      Mobile viewport resize width "
         "(default: 480, 0=disable)\n"
      << "  --tablet-width PIXELS      Tablet viewport resize width "
         "(default: 768, 0=disable)\n"
      << "  --desktop-width PIXELS     Desktop viewport resize width "
         "(default: 0=no resize)\n"
      << "  --jpeg-quality N           JPEG output quality 1-100 "
         "(default: 85)\n"
      << "  --webp-quality N           WebP output quality 0-100 "
         "(default: 75)\n"
      << "  --avif-quality N           AVIF output quality 0-100 "
         "(default: 60)\n"
      << "  --savedata-jpeg-quality N  Save-Data JPEG quality "
         "(default: 60)\n"
      << "  --savedata-webp-quality N  Save-Data WebP quality "
         "(default: 50)\n"
      << "  --savedata-avif-quality N  Save-Data AVIF quality "
         "(default: 45)\n"
      << "  --max-url-length BYTES     Max URL length "
         "(default: 8192)\n"
      << "  --max-html-size BYTES      Max HTML size "
         "(default: 5242880)\n"
      << "  --max-css-size BYTES       Max CSS size "
         "(default: 2097152)\n"
      << "  --max-js-size BYTES        Max JS size "
         "(default: 2097152)\n"
      << "  --max-image-size BYTES     Max image size "
         "(default: 10485760)\n"
      << "  --log-level LEVEL          Log level: "
         "debug|info|warning|error (default: info)\n"
      << "  --log-format FORMAT        Log format: "
         "text|json (default: text)\n"
      << kServiceUsage
      << "  --api-socket [PATH]        Serve the management API over a "
         "unix socket, mode 0660\n"
      << "                             (default path: "
         "/run/pagespeed-optimizer/api.sock)\n"
      << "  --api-port PORT            HTTP management API TCP port "
         "(default: 0=disabled)\n"
      << "  --api-bind ADDRESS         API bind address "
         "(default: 127.0.0.1)\n"
      << "  --api-allow-remote         Permit a non-loopback --api-bind "
         "(a token is still required)\n"
      << "  --api-no-auth              Permit a tokenless API on loopback "
         "or the unix socket\n"
      << "  --api-token TOKEN          API auth token "
         "(or set PAGESPEED_API_TOKEN env var)\n"
      << "  --api-read-open            Allow unauthenticated access to the "
         "allow-listed GET routes and the stats/events streams "
         "(public console reads; the log stays token-only)\n"
      << "  --no-security-headers      Omit security headers on console "
         "responses "
         "(reverse proxy handles them)\n"
      << "  --console-dir PATH         Web console SPA directory "
         "(optional)\n"
      << "  --enable-browser-analysis  Enable browser-based "
         "analysis (default: off)\n"
      << "  --browser-sandbox MODE     Headless Chrome sandbox: "
         "require|off (default: require;\n"
      << "                             `require` refuses browser analysis "
         "rather than running\n"
      << "                             an unsandboxed browser -- the daemon "
         "keeps serving)\n"
      << "  --browser-user-data-dir PATH  Chrome profile directory "
         "(default: <runtime dir>/chrome)\n"
      << "  --chrome-binary PATH       Chrome executable path "
         "(default: /usr/bin/chrome-headless-shell)\n"
      << "  --chrome-recycle-interval N Pages before Chrome "
         "restart (default: 100)\n"
      << "  --chrome-page-timeout MS   Per-page CDP timeout "
         "(default: 60000)\n"
      << "  --chrome-max-memory MB     Chrome RSS kill threshold "
         "(default: 512)\n"
      << "  --chrome-startup-timeout MS Chrome startup timeout "
         "(default: 10000)\n"
      << "  --no-browser-critical-css  Disable browser CSS "
         "(keep heuristic)\n"
      << "  --no-browser-lazy-loading  Disable browser fold "
         "detection\n"
      << "  --no-browser-lcp-preload   Disable browser LCP "
         "detection\n"
      << "  --no-browser-image-sizing  Disable browser image "
         "dimensions\n"
      << "  --browser-queue-size N     Max pending analysis "
         "items (default: 1000)\n"
      << "  --browser-profile-ttl SEC  Profile expiry in seconds "
         "(default: 86400)\n"
      << "  --allow-private-urls       Allow capture endpoints to "
         "target private/loopback URLs\n"
      << "\n"
      << "Cache Key Normalization:\n"
      << "  --strip-query-extensions EXT  Comma-separated extensions "
         "(e.g., .jpg,.png,.css)\n"
      << "  --strip-query-groups GROUPS   Comma-separated group names "
         "(e.g., images,static)\n"
      << "  --strip-query-params PARAMS   Comma-separated param names "
         "(e.g., utm_source,fbclid)\n"
      << "  --host-alias SRC=DST          Host alias mapping "
         "(repeatable)\n"
      << "\n"
      << "SVG Auto-Vectorization:\n"
      << "  --svg-mode MODE            detect, preview, or auto "
         "(default: detect)\n"
      << "  --svg-candidacy-threshold N  Score threshold 0-100 "
         "(default: 50)\n"
      << "  --svg-max-pixels N         Max decoded pixels "
         "(default: 65536)\n"
      << "  --svg-max-paths N          Max <path> elements "
         "(default: 500)\n"
      << "  --svg-max-svg-bytes N      Max uncompressed SVG bytes "
         "(default: 262144)\n"
      << "  --svg-fidelity-threshold F SSIMULACRA2 minimum "
         "(default: 55.0) [reserved]\n"
      << "  --svg-exclude-lcp BOOL     Skip LCP images "
         "(default: true)\n"
      << "  --svg-timeout-ms N         Vectorization timeout "
         "(default: 500) [reserved]\n"
      << "  --svg-preset N             0=bw, 1=poster, 2=photo "
         "(default: 1) [reserved]\n"
      << "  --svg-color-precision N    0=adaptive, 1-8=fixed "
         "(default: 0)\n"
      << "  --svg-filter-speckle N     Min cluster area "
         "(default: 4)\n"
      << "\n"
      << "Agent Optimize (EXPERIMENTAL — off by default):\n"
      << "  --agent-optimize           Serve a rendered-markdown variant to AI "
         "agents\n"
         "                             (Accept: text/markdown). Behavior may "
         "change.\n"
      << "  --agent-optimize-paths LIST   Comma-separated path prefixes to "
         "enable\n"
      << "  --agent-optimize-llms-txt  Publish a synthesized /llms.txt site "
         "index\n"
         "                             (requires --agent-optimize)\n"
      << "  --agent-optimize-sitemap-url URL  Sitemap to build /llms.txt from\n"
      << "  --[no-]agent-optimize-respect-ai-directives  Honor robots AI rules "
         "(default: on)\n"
      << "  --agent-optimize-llms-summary-fetch-cap N  Max summary fetches "
         "(default: 200)\n"
      << "  --agent-optimize-cache-ttl SEC  /llms.txt cache TTL "
         "(default: 86400)\n"
      << "  --agent-render-allow-hosts HOSTS  Third-party hosts the agent "
         "render may\n"
         "                             fetch: comma-separated, exact host "
         "names (no\n"
         "                             wildcards), still SSRF-guarded "
         "(default: none)\n"
      << "  --agent-render-private-origins ORIGINS  Own origins the agent "
         "render may\n"
         "                             fetch at a loopback or private "
         "address:\n"
         "                             comma-separated scheme://host[:port] "
         "or\n"
         "                             host[:port], no wildcards; "
         "repeatable\n"
         "                             (default: none)\n"
      << "\n"
      << "Web Bot Auth (observe-only — off by default):\n"
      << "  --web-bot-auth             Classify RFC 9421-signed agent "
         "requests\n"
         "                             (label + counter only; never changes "
         "request\n"
         "                             handling)\n"
      << "  --web-bot-auth-key-directory URL  https JWKS key-directory URL "
         "to\n"
         "                             warm-fetch periodically (repeatable)\n"
      << "  --web-bot-auth-verified-bots LIST  keyid=name pairs "
         "(comma-separated)\n"
         "                             promoting a verified signature to a "
         "named bot\n"
      << "  --web-bot-auth-public-counter MODE  opt-in verified-crawl counter "
         "(experimental):\n"
         "                             off (default) | private | public.  "
         "Enabling a\n"
         "                             non-off mode publishes a discoverable "
         "marker at\n"
         "                             /.well-known/webbotauth-counter.  The "
         "exact doc is\n"
         "                             gated by the "
         "PAGESPEED_WEB_BOT_AUTH_COUNTER_TOKEN env\n"
         "                             bearer token.\n"
      << "\n"
      << "RSL-CAP Enforcement (EXPERIMENTAL — off by default):\n"
      << "  --rsl-cap-enforcement      Validate Authorization: License "
         "capability tokens\n"
         "                             and answer 401, 402 or pass (status "
         "codes only;\n"
         "                             never settles or meters payment)\n"
      << "  --rsl-cap-key-directory URL  https JWKS key-directory URL of a "
         "token issuer\n"
         "                             to warm-fetch periodically "
         "(repeatable)\n"
      << "  --rsl-cap-requested-license ID  License id a token must grant "
         "(empty: every\n"
         "                             valid token is refused with 402)\n"
      << "  --rsl-cap-requested-scope SCOPE  Scope a token must grant (empty: "
         "every valid\n"
         "                             token is refused with 402)\n"
      << "  --rsl-cap-issuer ISSUER    Accept tokens from this issuer only; "
         "others are\n"
         "                             rejected as an unknown issuer (401). "
         "Empty: any\n"
         "                             issuer in the key directories\n"
      << "\n"
      << "  --version                  Show version and exit\n"
      << "  --help                     Show this help\n";
}

// The cache volume stem inside a cache directory: the volume file(s), the
// shared config and the serve-stats mmap all live beside this stem.  The
// stem is deliberately extensionless (see deploy/pagespeed-optimizer.service
// for why an extensioned stem breaks the module's volume matcher).
std::string CachePathFromDir(const std::string& dir) { return dir + "/cache"; }

// The compiled-in default cache directory, with a generation suffix
// (/var/cache/pagespeed-optimizer/v<N>, N = kCacheDirGeneration).
std::string DefaultCacheDir() {
  return absl::StrCat(pagespeed::kDefaultCacheDirPrefix,
                      pagespeed::kCacheDirGeneration);
}

}  // namespace

// A secret passed on the command line is readable by every other local
// account for as long as the process lives.  The packaged unit supplies both
// through an environment file instead; say so once, loudly, and never echo
// the value itself.
void WarnSecretOnCommandLine(const char* flag, const char* env_var) {
  std::cerr << "WARNING: " << flag
            << " was passed on the command line, where its value is visible "
               "to other local accounts on this host for the lifetime of the "
               "process. Set "
            << env_var
            << " instead (the packaged daemon reads it from "
               "/etc/pagespeed-optimizer/daemon.env, mode 0640).\n";
}

// A 2.0 licensing setting (flag or environment variable) that 2.1 no longer
// has.  Accepted and ignored so an unchanged 2.0 unit file, env
// file or wrapper script still starts the daemon; one WARNING per setting
// names the change so the operator can delete it.  The value is never used
// and never echoed.
void WarnRetiredLicensingSetting(const char* setting) {
  std::cerr << "WARNING: " << setting
            << " was removed in 2.1 and is ignored. Remove the setting from "
               "your configuration.\n";
}

// Outcome of matching one command-line option in ParseApiOption.
enum class OptionMatch : std::uint8_t {
  kNotMine,   // not one of ours; fall through to the main option chain
  kConsumed,  // handled (and any value argument consumed)
  kError,     // handled and invalid; the reason has been printed
};

// The management-API and headless-browser-sandbox options, matched in one
// place rather than as more links in main()'s option chain (see the call site
// for why MSVC forces that).  `*i` is advanced past a consumed value.
OptionMatch ParseApiOption(std::string_view arg, int* i, int argc, char* argv[],
                           pagespeed::WorkerConfig& config) {
  const bool has_value = (*i + 1 < argc);
  if (arg == "--api-port" && has_value) {
    config.api_port = std::stoi(argv[++(*i)]);
    if (config.api_port < 0 || config.api_port > 65535) {
      std::cerr << "Error: --api-port must be 0-65535 (0 = disabled)\n";
      return OptionMatch::kError;
    }
    return OptionMatch::kConsumed;
  }
  if (arg == "--api-bind" && has_value) {
    config.api_bind_address = argv[++(*i)];
    return OptionMatch::kConsumed;
  }
  if (arg == "--api-socket") {
    // Optional value: `--api-socket` alone takes the packaged default path,
    // `--api-socket PATH` takes PATH.  A following token starting with '-'
    // is the next flag, not a socket path.
    if (has_value && argv[*i + 1][0] != '-') {
      config.api_socket_path = argv[++(*i)];
    } else {
      config.api_socket_path = pagespeed::kDefaultApiSocketPath;
    }
    return OptionMatch::kConsumed;
  }
  if (arg == "--api-allow-remote") {
    config.api_allow_remote = true;
    return OptionMatch::kConsumed;
  }
  if (arg == "--api-no-auth") {
    config.api_no_auth = true;
    return OptionMatch::kConsumed;
  }
  if (arg == "--api-token" && has_value) {
    WarnSecretOnCommandLine("--api-token", "PAGESPEED_API_TOKEN");
    config.api_token = argv[++(*i)];
    return OptionMatch::kConsumed;
  }
  if (arg == "--api-read-open") {
    config.api_read_open = true;
    return OptionMatch::kConsumed;
  }
  if (arg == "--browser-sandbox" && has_value) {
    const std::string mode = argv[++(*i)];
    if (!pagespeed::ParseBrowserSandboxMode(
            mode, &config.browser_analysis.sandbox_mode)) {
      std::cerr << "Error: --browser-sandbox must be 'require' or 'off' (got '"
                << mode
                << "'). There is deliberately no 'auto': a mode that silently "
                   "falls back to an unsandboxed browser is the defect this "
                   "flag exists to remove.\n";
      return OptionMatch::kError;
    }
    return OptionMatch::kConsumed;
  }
  if (arg == "--browser-user-data-dir" && has_value) {
    config.browser_analysis.chrome_user_data_dir = argv[++(*i)];
    return OptionMatch::kConsumed;
  }
  return OptionMatch::kNotMine;
}

// PAGESPEED_BROWSER_SANDBOX as the env fallback for --browser-sandbox.
// Returns false (after printing why) when the value is not require|off.
bool ResolveBrowserSandboxMode(pagespeed::WorkerConfig& config) {
  const char* env_sandbox = std::getenv("PAGESPEED_BROWSER_SANDBOX");
  if (env_sandbox == nullptr || env_sandbox[0] == '\0') return true;
  if (pagespeed::ParseBrowserSandboxMode(
          env_sandbox, &config.browser_analysis.sandbox_mode)) {
    return true;
  }
  std::cerr << "Error: PAGESPEED_BROWSER_SANDBOX must be 'require' or 'off' "
               "(got '"
            << env_sandbox << "')\n";
  return false;
}

// The management API invariant, enforced HERE, at config
// parse, with a fatal named refusal.  Never a warning, never a silent
// downgrade:
//
//   REMOTE IS NEVER UNAUTHENTICATED, AND UNAUTHENTICATED IS NEVER REMOTE.
//
// The refusal names the exact flag that would make the requested posture
// legal, so an operator who really wants it is one documented flag away and
// an operator who did not mean it finds out at startup rather than from a
// stranger purging their cache.  Returns false to refuse the start.
bool ValidateManagementApiConfig(const pagespeed::WorkerConfig& config) {
  const bool has_socket = !config.api_socket_path.empty();
  const bool has_tcp = config.api_port != 0;
  if (!has_socket && !has_tcp) return true;  // API disabled

  // One transport.  Accepting both and quietly serving
  // only one would leave an operator reading a config that says something the
  // daemon is not doing -- and if the dropped one were the socket, the module
  // would sit on a path nothing is listening at.
  if (has_socket && has_tcp) {
    std::cerr << "Error: refusing to start -- --api-socket and --api-port both "
                 "select a management-API transport, and the daemon serves "
                 "exactly one. Keep --api-socket for local access (no "
                 "credential needed), or --api-port for a TCP listener.\n";
    return false;
  }

  // The unix socket carries its own boundary: mode 0660, group `pagespeed`,
  // the same scope as the cache volume and the management socket.  Reaching
  // it IS the credential (§13 Q3), so there is nothing further to check --
  // no token to require, no remote to permit, nothing to opt out of.
  if (has_socket) return true;

  // From here it is TCP.
  if (!pagespeed::IsIpv4LiteralApiBind(config.api_bind_address)) {
    std::cerr << "Error: refusing to start -- --api-bind '"
              << config.api_bind_address
              << "' is not an IPv4 literal. The management API binds IPv4 "
                 "only; IPv6 and hostnames (including 'localhost' and '::1') "
                 "are not supported. Use 127.0.0.1 for local access, or "
                 "--api-socket, which binds no port at all.\n";
    return false;
  }

  const bool remote = !pagespeed::IsLoopbackApiBind(config.api_bind_address);
  const bool has_token = !config.api_token.empty();
  const char* bind_shown = config.api_bind_address.empty()
                               ? "127.0.0.1"
                               : config.api_bind_address.c_str();

  if (remote && !has_token) {
    std::cerr << "Error: refusing to start -- the management API is bound to "
                 "the non-loopback address "
              << bind_shown
              << " with no token. A remotely reachable management API is never "
                 "allowed to be unauthenticated, whatever else is set. Set "
                 "PAGESPEED_API_TOKEN (the package generates one into "
                 "/etc/pagespeed-optimizer/daemon.env), or bind loopback, or "
                 "use --api-socket.\n";
    return false;
  }
  if (remote && !config.api_allow_remote) {
    std::cerr << "Error: refusing to start -- the management API is bound to "
                 "the non-loopback address "
              << bind_shown
              << ". Publishing the management API off-host is deliberate: pass "
                 "--api-allow-remote (PAGESPEED_API_ALLOW_REMOTE=true) to "
                 "confirm, and put it behind your reverse proxy's own "
                 "authentication as well. To keep it local, bind 127.0.0.1 or "
                 "use --api-socket.\n";
    return false;
  }
  if (!has_token && !config.api_no_auth) {
    std::cerr << "Error: refusing to start -- the management API is enabled on "
                 "TCP with no token. An unauthenticated API answers cache "
                 "purge, the cached-URL inventory and the running "
                 "configuration to anyone who can reach it. Set "
                 "PAGESPEED_API_TOKEN, or use --api-socket (whose 0660 group "
                 "scope is the credential, so no token is needed), or -- if "
                 "loopback on a single-tenant host really is the boundary -- "
                 "pass --api-no-auth (PAGESPEED_API_NO_AUTH=true) to say so "
                 "deliberately.\n";
    return false;
  }

  // Legal, but worth a banner: both opt-outs weaken a default.
  if (config.api_allow_remote && remote) {
    std::cerr << "WARNING: the management API is published on " << bind_shown
              << " (--api-allow-remote). It is authenticated, but it is "
                 "reachable from off-host -- keep it behind a reverse proxy "
                 "and rotate the token.\n";
  }
  if (config.api_no_auth) {
    std::cerr << "WARNING: the management API is running WITHOUT a token "
                 "(--api-no-auth). Anyone who can reach "
              << bind_shown
              << " can purge the cache and read the cached-URL inventory.\n";
  }
  return true;
}

// The worker's whole startup and run: the process entry point on a console,
// or the service's body under the Windows service dispatcher (`--service`).
static int WorkerMain(int argc,  // NOLINT(bugprone-exception-escape)
                      char* argv[]) {
  // Syscall-filter positive control, FIRST: it deliberately makes a call the
  // enforcing syscall profile denies, and the point is to make it before any
  // startup work could fail for an unrelated reason.  Compiled out of every
  // shipped binary -- in a release build this is `return false` and the flag
  // string is not in the binary, so `--selftest-forbidden-syscall` falls
  // through to the parser below and is reported as an unknown option.
  {
    int selftest_exit = 0;
    if (pagespeed::MaybeRunSyscallSelftest(argc, argv, &selftest_exit)) {
      return selftest_exit;
    }
  }

#ifndef _WIN32
  // Ignore SIGPIPE for the whole process, before any thread or the event
  // loop exists.  Every listener (the health, management and notification
  // sockets, the HTTP API and its WebSocket streams) and the browser pipe
  // write through libuv, which does not suppress the signal per call on
  // Linux; left at its default disposition, a peer that closed its end before
  // the reply was written -- a health check that gave up, a client that
  // disconnected mid-response, a browser that exited -- terminated the whole
  // daemon.  Ignored, the same write returns EPIPE, which every write path
  // already answers by closing that one connection.  The same holds for
  // stdout/stderr when they are a pipe whose reader went away.
  //
  // An ignored disposition survives execve, so it matters how children are
  // started: the browser is spawned through libuv, which resets every
  // disposition to the default in the child, and the curl helper's spawn
  // attributes reset this one explicitly (src/browser/agent_fetcher.cc).
  // Windows has no such signal.
  std::signal(SIGPIPE, SIG_IGN);
#endif

  pagespeed::WorkerConfig config;
  std::string log_level_str = "info";
  std::string log_format_str = "text";
  std::string cache_dir_arg;

  // Pass 1: Extract --cache-path / --cache-dir to locate the config file.
  // --cache-path is the expert override and wins over --cache-dir wherever
  // both appear; with neither, the compiled-in default directory applies.
  for (int i = 1; i < argc; ++i) {
    if (std::string_view(argv[i]) == "--cache-path" && i + 1 < argc) {
      config.cache_path = argv[i + 1];
      break;
    }
    if (std::string_view(argv[i]) == "--cache-dir" && i + 1 < argc) {
      cache_dir_arg = argv[i + 1];
    }
  }
  if (config.cache_path.empty()) {
    config.cache_path = CachePathFromDir(
        cache_dir_arg.empty() ? DefaultCacheDir() : cache_dir_arg);
  }

  // Load persisted config file (defaults -> file -> env -> CLI).
  if (!config.cache_path.empty()) {
    std::string cfg_path = pagespeed::ConfigFilePath(config.cache_path);
    std::string cfg_error;
    auto file_json = pagespeed::ReadConfigFile(cfg_path, &cfg_error);
    if (!cfg_error.empty()) {
      std::cerr << "Warning: Config file " << cfg_path << ": " << cfg_error
                << "\n";
    }
    if (!file_json.empty()) {
      auto cr = pagespeed::ApplyConfigJson(file_json, &config);
      for (auto& [key, reason] : cr.rejected.items()) {
        std::cerr << "Warning: Config file: rejected " << key << ": " << reason
                  << "\n";
      }
      for (auto& w : cr.warnings) {
        std::cerr << "Warning: Config file: " << w << "\n";
      }
    }
  }

  // Pass 2: Full CLI parsing. CLI flags override config file values.
  try {
    for (int i = 1; i < argc; ++i) {
      std::string arg = argv[i];
      // The management-API and browser-sandbox options are matched HERE,
      // ahead of the chain below.  That chain is one `else if` per option and
      // MSVC counts each as another nested block, giving up at 128 (C1061) --
      // it was already within a handful of the limit, so a group of new
      // options cannot go in it.  Keeping this surface in one function is
      // also how it stays readable.
      const OptionMatch api_match = ParseApiOption(arg, &i, argc, argv, config);
      if (api_match == OptionMatch::kError) return 1;
      if (api_match == OptionMatch::kConsumed) continue;
      if (arg == "--socket" && i + 1 < argc) {
        config.socket_path = argv[++i];
      } else if (arg == "--cache-path" && i + 1 < argc) {
        config.cache_path = argv[++i];
      } else if (arg == "--cache-dir" && i + 1 < argc) {
        cache_dir_arg = argv[++i];
      } else if (arg == "--cache-size" && i + 1 < argc) {
        config.cache_size_bytes = std::stoull(argv[++i]);
        if (config.cache_size_bytes == 0) {
          std::cerr << "Error: --cache-size must be > 0\n";
          return 1;
        }
      } else if (arg == "--ram-cache-size" && i + 1 < argc) {
        config.ram_cache_size = std::stoull(argv[++i]);
      } else if (arg == "--read-lease-duration" && i + 1 < argc) {
        config.read_lease_duration_ms = std::stoi(argv[++i]);
        if (config.read_lease_duration_ms < 0) {
          std::cerr << "Error: --read-lease-duration must be >= 0\n";
          return 1;
        }
      } else if (arg == "--lease-wrap-ceiling" && i + 1 < argc) {
        config.lease_wrap_ceiling_ms = std::stoi(argv[++i]);
        if (config.lease_wrap_ceiling_ms < 1) {
          std::cerr << "Error: --lease-wrap-ceiling must be >= 1\n";
          return 1;
        }
      } else if (arg == "--num-threads" && i + 1 < argc) {
        config.num_threads = std::stoi(argv[++i]);
        if (config.num_threads < 0) {
          std::cerr << "Error: --num-threads must be >= 0\n";
          return 1;
        }
      } else if (arg == "--max-connections" && i + 1 < argc) {
        config.max_connections = std::stoi(argv[++i]);
        if (config.max_connections < 1) {
          std::cerr << "Error: --max-connections must be >= 1\n";
          return 1;
        }
      } else if (arg == "--max-buffer-size" && i + 1 < argc) {
        config.max_buffer_size = std::stoull(argv[++i]);
      } else if (arg == "--connection-timeout" && i + 1 < argc) {
        config.connection_timeout_ms = std::stoi(argv[++i]);
        if (config.connection_timeout_ms < 0) {
          std::cerr << "Error: --connection-timeout must be >= 0\n";
          return 1;
        }
      } else if (arg == "--shutdown-timeout" && i + 1 < argc) {
        config.shutdown_timeout_ms = std::stoi(argv[++i]);
        if (config.shutdown_timeout_ms < 0) {
          std::cerr << "Error: --shutdown-timeout must be >= 0\n";
          return 1;
        }
      } else if (arg == "--disable-html") {
        config.disable_html = true;
      } else if (arg == "--disable-css") {
        config.disable_css = true;
      } else if (arg == "--disable-js") {
        config.disable_js = true;
      } else if (arg == "--disable-image") {
        config.disable_image = true;
      } else if (arg == "--proactive-image-variants") {
        config.proactive_image_variants = true;
      } else if (arg == "--no-proactive-viewport-variants") {
        config.proactive_viewport_variants = false;
      } else if (arg == "--no-proactive-savedata-variants") {
        config.proactive_savedata_variants = false;
      } else if (arg == "--no-proactive-density-variants") {
        config.proactive_density_variants = false;
      } else if (arg == "--enable-warmup") {
        config.enable_warmup = true;
      } else if (arg == "--no-lazy-load-images") {
        config.disable_lazy_load = true;
      } else if (arg == "--no-image-dimensions") {
        config.disable_image_dimensions = true;
      } else if (arg == "--no-lcp-preload") {
        config.disable_lcp_preload = true;
      } else if (arg == "--no-preconnect-injection") {
        config.disable_preconnect_injection = true;
      } else if (arg == "--no-async-css") {
        config.disable_async_css = true;
      } else if (arg == "--async-css-min-coverage" && i + 1 < argc) {
        if (!ParseFloat("--async-css-min-coverage", argv[++i], 0.0f, 1.0f,
                        &config.async_css_min_coverage)) {
          return 1;
        }
      } else if (arg == "--async-css-min-deferred-bytes" && i + 1 < argc) {
        config.async_css_min_deferred_bytes =
            static_cast<size_t>(std::stoul(argv[++i]));
      } else if (pagespeed::ParseUnsafeForceAsyncCssFlag(
                     arg, &config.unsafe_force_async_css)) {
        // Undocumented on purpose — not in PrintUsage, not in the user docs.
      } else if (arg == "--enable-speculation-rules") {
        config.enable_speculation_rules = true;
      } else if (arg == "--no-content-analysis") {
        config.content_analysis = false;
      } else if (arg == "--no-preserve-c2pa") {
        config.preserve_c2pa = false;
      } else if (arg == "--c2pa-carry") {
        config.c2pa_carry = true;
      } else if ((arg == "--denoise-threshold" ||
                  arg == "--denoise-sigma-spatial" ||
                  arg == "--denoise-sigma-range") &&
                 i + 1 < argc) {
        // Removed options, accepted and ignored so existing command lines
        // keep working.
        ++i;
        std::cerr << "warning: " << arg << " is deprecated and ignored\n";
      } else if (arg == "--no-quality-verify") {
        config.quality_verify = false;
      } else if (arg == "--target-ssimulacra2" && i + 1 < argc) {
        if (!ParseFloat("--target-ssimulacra2", argv[++i], 0.0f, 100.0f,
                        &config.target_ssimulacra2))
          return 1;
      } else if (arg == "--ssimulacra2-tolerance" && i + 1 < argc) {
        if (!ParseFloat("--ssimulacra2-tolerance", argv[++i], 0.0f, 50.0f,
                        &config.ssimulacra2_tolerance))
          return 1;
      } else if (arg == "--no-learned-quality") {
        config.learned_quality = false;
      } else if (arg == "--no-learned-quality-jpeg") {
        config.learned_quality_jpeg = false;
      } else if (arg == "--no-learned-quality-webp") {
        config.learned_quality_webp = false;
      } else if (arg == "--no-learned-quality-avif") {
        config.learned_quality_avif = false;
      } else if (arg == "--savedata-score-reduction" && i + 1 < argc) {
        if (!ParseFloat("--savedata-score-reduction", argv[++i], 0.0f, 50.0f,
                        &config.savedata_score_reduction))
          return 1;
      } else if (arg == "--gzip-level" && i + 1 < argc) {
        config.gzip_level = std::stoi(argv[++i]);
        if (config.gzip_level < 0 || config.gzip_level > 9) {
          std::cerr << "Error: --gzip-level must be 0-9\n";
          return 1;
        }
      } else if (arg == "--brotli-level" && i + 1 < argc) {
        config.brotli_level = std::stoi(argv[++i]);
        if (config.brotli_level < 0 || config.brotli_level > 11) {
          std::cerr << "Error: --brotli-level must be 0-11\n";
          return 1;
        }
      } else if (arg == "--no-css-import-flattening") {
        config.disable_css_import_flattening = true;
      } else if (arg == "--mobile-width" && i + 1 < argc) {
        config.mobile_width = static_cast<uint32_t>(std::stoul(argv[++i]));
        if (config.mobile_width < 160 || config.mobile_width > 10000) {
          std::cerr << "Error: --mobile-width must be 160-10000\n";
          return 1;
        }
      } else if (arg == "--tablet-width" && i + 1 < argc) {
        config.tablet_width = static_cast<uint32_t>(std::stoul(argv[++i]));
        if (config.tablet_width < 160 || config.tablet_width > 10000) {
          std::cerr << "Error: --tablet-width must be 160-10000\n";
          return 1;
        }
      } else if (arg == "--desktop-width" && i + 1 < argc) {
        config.desktop_width = static_cast<uint32_t>(std::stoul(argv[++i]));
        if (config.desktop_width < 160 || config.desktop_width > 10000) {
          std::cerr << "Error: --desktop-width must be 160-10000\n";
          return 1;
        }
      } else if (arg == "--jpeg-quality" && i + 1 < argc) {
        config.jpeg_quality = std::stoi(argv[++i]);
      } else if (arg == "--webp-quality" && i + 1 < argc) {
        config.webp_quality = std::stoi(argv[++i]);
      } else if (arg == "--avif-quality" && i + 1 < argc) {
        config.avif_quality = std::stoi(argv[++i]);
      } else if (arg == "--savedata-jpeg-quality" && i + 1 < argc) {
        config.savedata_jpeg_quality = std::stoi(argv[++i]);
      } else if (arg == "--savedata-webp-quality" && i + 1 < argc) {
        config.savedata_webp_quality = std::stoi(argv[++i]);
      } else if (arg == "--savedata-avif-quality" && i + 1 < argc) {
        config.savedata_avif_quality = std::stoi(argv[++i]);
      } else if (arg == "--max-url-length" && i + 1 < argc) {
        config.max_url_length = std::stoull(argv[++i]);
      } else if (arg == "--max-html-size" && i + 1 < argc) {
        config.max_html_size = std::stoull(argv[++i]);
      } else if (arg == "--max-css-size" && i + 1 < argc) {
        config.max_css_size = std::stoull(argv[++i]);
      } else if (arg == "--max-js-size" && i + 1 < argc) {
        config.max_js_size = std::stoull(argv[++i]);
      } else if (arg == "--max-image-size" && i + 1 < argc) {
        config.max_image_size = std::stoull(argv[++i]);
      } else if (arg == "--log-level" && i + 1 < argc) {
        log_level_str = argv[++i];
      } else if (arg == "--log-format" && i + 1 < argc) {
        log_format_str = argv[++i];
      } else if ((arg == "--license-key" || arg == "--license-renewal-url" ||
                  arg == "--fastspring-storefront" ||
                  arg == "--fastspring-product") &&
                 i + 1 < argc) {
        // Retired 2.0 licensing flags.  Consume the value so an
        // unchanged 2.0 unit file or wrapper script still starts the daemon;
        // say once what changed; never echo the value.
        ++i;
        WarnRetiredLicensingSetting(arg.c_str());
      } else if (arg == "--web-bot-auth") {
        // Enable the observe-only Web Bot Auth classifier.
        // OFF by default; the verdict never changes request handling.
        config.web_bot_auth = true;
      } else if (arg == "--web-bot-auth-key-directory" && i + 1 < argc) {
        // Repeatable. https JWKS directory URL, e.g.
        // https://example.com/.well-known/http-message-signatures-directory
        std::string url = argv[++i];
        if (!url.starts_with("https://")) {
          std::cerr << "Error: --web-bot-auth-key-directory must be an "
                       "https:// URL\n";
          return 1;
        }
        config.web_bot_auth_key_directories.push_back(std::move(url));
      } else if (arg == "--web-bot-auth-verified-bots" && i + 1 < argc) {
        // "keyid=name,keyid2=name2" — operator-curated promotion map.
        config.web_bot_auth_verified_bots = argv[++i];
      } else if (arg == "--web-bot-auth-public-counter" && i + 1 < argc) {
        // Experimental: opt-in verified-crawl counter mode.
        // off | private | public.  Anything else is rejected.
        std::string mode = argv[++i];
        if (mode != "off" && mode != "private" && mode != "public") {
          std::cerr << "Error: --web-bot-auth-public-counter must be one of "
                       "off, private, public\n";
          return 1;
        }
        config.web_bot_auth_public_counter = std::move(mode);
      } else if (arg == "--rsl-cap-enforcement") {
        // Experimental: enable the RSL-CAP capability-token
        // enforcement handler.  OFF by default; validates
        // Authorization: License tokens and returns 401/402/pass (status only).
        config.rsl_cap_enforcement = true;
      } else if (arg == "--rsl-cap-key-directory" && i + 1 < argc) {
        // Repeatable. https JWKS directory URL for the token issuers, warmed
        // into the enforcement realm "rsl".
        std::string url = argv[++i];
        if (!url.starts_with("https://")) {
          std::cerr << "Error: --rsl-cap-key-directory must be an "
                       "https:// URL\n";
          return 1;
        }
        config.rsl_cap_key_directories.push_back(std::move(url));
      } else if (arg == "--rsl-cap-requested-license" && i + 1 < argc) {
        config.rsl_cap_requested_license = argv[++i];
      } else if (arg == "--rsl-cap-requested-scope" && i + 1 < argc) {
        config.rsl_cap_requested_scope = argv[++i];
      } else if (arg == "--rsl-cap-issuer" && i + 1 < argc) {
        config.rsl_cap_issuer = argv[++i];
      } else if (arg == "--no-security-headers") {
        config.security_headers = false;
      } else if (arg == "--console-dir" && i + 1 < argc) {
        config.console_dir = argv[++i];
      } else if (arg == "--enable-browser-analysis") {
        config.browser_analysis.enabled = true;
      } else if (arg == "--chrome-binary" && i + 1 < argc) {
        config.browser_analysis.chrome_binary = argv[++i];
      } else if (arg == "--chrome-recycle-interval" && i + 1 < argc) {
        config.browser_analysis.chrome_recycle_interval = std::stoi(argv[++i]);
      } else if (arg == "--chrome-page-timeout" && i + 1 < argc) {
        config.browser_analysis.chrome_page_timeout_ms = std::stoi(argv[++i]);
      } else if (arg == "--chrome-max-memory" && i + 1 < argc) {
        config.browser_analysis.chrome_max_memory_mb = std::stoi(argv[++i]);
      } else if (arg == "--chrome-startup-timeout" && i + 1 < argc) {
        config.browser_analysis.chrome_startup_timeout_ms =
            std::stoi(argv[++i]);
      } else if (arg == "--no-browser-critical-css") {
        config.browser_analysis.enable_browser_critical_css = false;
      } else if (arg == "--no-browser-lazy-loading") {
        config.browser_analysis.enable_browser_lazy_loading = false;
      } else if (arg == "--no-browser-lcp-preload") {
        config.browser_analysis.enable_browser_lcp_preload = false;
      } else if (arg == "--no-browser-image-sizing") {
        config.browser_analysis.enable_browser_image_sizing = false;
      } else if (arg == "--browser-queue-size" && i + 1 < argc) {
        config.browser_analysis.browser_queue_size = std::stoull(argv[++i]);
      } else if (arg == "--browser-profile-ttl" && i + 1 < argc) {
        config.browser_analysis.browser_profile_ttl_seconds =
            std::stoll(argv[++i]);
      } else if (arg == "--agent-optimize") {
        // Enable the agent_optimize render + the serve-side
        // markdown toggle.  OFF by default; operator flag only.
        config.browser_analysis.agent_optimize = true;
      } else if (arg == "--agent-render-allow-hosts" && i + 1 < argc) {
        // Comma-separated opt-in third-party hosts the agent render may fetch
        // (through the G2 SSRF guard). Surrounding whitespace is trimmed so a
        // natural "a.com, b.com" list works; empty tokens are skipped.
        std::string hosts = argv[++i];
        size_t pos = 0;
        while (pos < hosts.size()) {
          size_t comma = hosts.find(',', pos);
          if (comma == std::string::npos) comma = hosts.size();
          size_t start = pos;
          size_t end = comma;
          while (start < end &&
                 std::isspace(static_cast<unsigned char>(hosts[start]))) {
            ++start;
          }
          while (end > start &&
                 std::isspace(static_cast<unsigned char>(hosts[end - 1]))) {
            --end;
          }
          if (end > start) {
            config.browser_analysis.agent_render_allow_hosts.push_back(
                hosts.substr(start, end - start));
          }
          pos = comma + 1;
        }
      } else if (arg == "--agent-render-private-origins" && i + 1 < argc) {
        // Comma-separated own origins ("scheme://host[:port]" or a bare
        // "host[:port]" for both schemes, no wildcards) the agent render and
        // /llms.txt may fetch although they resolve to a loopback or private
        // address.  Repeatable: each use adds to the list.
        std::vector<std::string> origins =
            absl::StrSplit(argv[++i], ',', absl::SkipWhitespace());
        if (!pagespeed::ParseOperatorOrigins(origins).has_value()) {
          std::cerr << "Error: --agent-render-private-origins takes "
                       "comma-separated origins (scheme://host[:port] or "
                       "host[:port], no wildcards)\n";
          return 1;
        }
        auto& list = config.browser_analysis.agent_render_private_origins;
        for (std::string& o : origins) {
          std::string_view t = absl::StripAsciiWhitespace(o);
          if (!t.empty()) list.emplace_back(t);
        }
      } else if (arg == "--agent-optimize-llms-txt") {
        // Enable the synthesized /llms.txt site-index. OFF by
        // default; also requires --agent-optimize.
        config.browser_analysis.agent_optimize_llms_txt = true;
      } else if (arg == "--no-agent-optimize-respect-ai-directives") {
        config.browser_analysis.agent_optimize_respect_ai_directives = false;
      } else if (arg == "--agent-optimize-respect-ai-directives") {
        config.browser_analysis.agent_optimize_respect_ai_directives = true;
      } else if (arg == "--agent-optimize-sitemap-url" && i + 1 < argc) {
        // Own-origin path only (e.g. "/sitemap.xml"). The builder always fetches
        // it against the request's own origin; an absolute third-party URL is
        // ineffective because the G1 pin rejects off-origin fetches.
        config.browser_analysis.agent_optimize_sitemap_url = argv[++i];
      } else if (arg == "--agent-optimize-llms-summary-fetch-cap" &&
                 i + 1 < argc) {
        // Reject negatives explicitly: std::stoull silently wraps "-1" to a huge
        // size_t rather than throwing, which would uncap per-page fetches.
        std::string v = argv[++i];
        if (v.find('-') != std::string::npos) {
          std::cerr << "Error: --agent-optimize-llms-summary-fetch-cap must be "
                       "a non-negative integer\n";
          return 1;
        }
        config.browser_analysis.agent_optimize_llms_summary_fetch_cap =
            std::stoull(v);
      } else if (arg == "--agent-optimize-cache-ttl" && i + 1 < argc) {
        config.browser_analysis.agent_optimize_cache_ttl_seconds =
            std::stoll(argv[++i]);
      } else if (arg == "--agent-optimize-paths" && i + 1 < argc) {
        // Comma-separated path patterns scoping which sitemap URLs are eligible
        // for the /llms.txt index ("/" or empty = all). Whitespace-trimmed;
        // empty tokens skipped.
        std::string paths = argv[++i];
        config.browser_analysis.agent_optimize_paths.clear();
        size_t pos = 0;
        while (pos < paths.size()) {
          size_t comma = paths.find(',', pos);
          if (comma == std::string::npos) comma = paths.size();
          size_t start = pos;
          size_t end = comma;
          while (start < end &&
                 std::isspace(static_cast<unsigned char>(paths[start]))) {
            ++start;
          }
          while (end > start &&
                 std::isspace(static_cast<unsigned char>(paths[end - 1]))) {
            --end;
          }
          if (end > start) {
            config.browser_analysis.agent_optimize_paths.push_back(
                paths.substr(start, end - start));
          }
          pos = comma + 1;
        }
      } else if (arg == "--svg-mode" && i + 1 < argc) {
        std::string mode = argv[++i];
        if (mode == "detect") {
          config.svg_mode = pagespeed::SvgMode::kDetect;
        } else if (mode == "preview") {
          config.svg_mode = pagespeed::SvgMode::kPreview;
        } else if (mode == "auto") {
          config.svg_mode = pagespeed::SvgMode::kAuto;
        } else {
          fprintf(stderr, "Unknown --svg-mode: %s (use detect/preview/auto)\n",
                  mode.c_str());
          return 1;
        }
      } else if (arg == "--svg-candidacy-threshold" && i + 1 < argc) {
        config.svg_candidacy_threshold =
            std::clamp(std::stoi(argv[++i]), 0, 100);
      } else if (arg == "--svg-max-pixels" && i + 1 < argc) {
        config.svg_max_pixels = std::clamp(std::stoi(argv[++i]), 1, 16777216);
      } else if (arg == "--svg-max-paths" && i + 1 < argc) {
        config.svg_max_paths = std::clamp(std::stoi(argv[++i]), 1, 100000);
      } else if (arg == "--svg-fidelity-threshold" && i + 1 < argc) {
        config.svg_fidelity_threshold =
            std::clamp(std::stof(argv[++i]), 0.0f, 100.0f);
      } else if (arg == "--svg-exclude-lcp" && i + 1 < argc) {
        std::string val = argv[++i];
        config.svg_exclude_lcp = (val == "true" || val == "1");
      } else if (arg == "--svg-timeout-ms" && i + 1 < argc) {
        config.svg_timeout_ms = std::clamp(std::stoi(argv[++i]), 10, 60000);
      } else if (arg == "--svg-preset" && i + 1 < argc) {
        config.svg_preset = std::clamp(std::stoi(argv[++i]), 0, 2);
      } else if (arg == "--svg-color-precision" && i + 1 < argc) {
        config.svg_color_precision = std::clamp(std::stoi(argv[++i]), 0, 8);
      } else if (arg == "--svg-filter-speckle" && i + 1 < argc) {
        config.svg_filter_speckle = std::clamp(std::stoi(argv[++i]), 0, 1000);
      } else if (arg == "--svg-max-svg-bytes" && i + 1 < argc) {
        config.svg_max_svg_bytes =
            std::clamp(std::stoi(argv[++i]), 1024, 16777216);
      } else if (arg == "--strip-query-extensions" && i + 1 < argc) {
        std::string val = argv[++i];
        config.strip_query_extensions.clear();
        size_t start = 0;
        while (start < val.size()) {
          size_t comma = val.find(',', start);
          if (comma == std::string::npos) comma = val.size();
          std::string token = val.substr(start, comma - start);
          if (!token.empty()) config.strip_query_extensions.push_back(token);
          start = comma + 1;
        }
      } else if (arg == "--strip-query-groups" && i + 1 < argc) {
        std::string val = argv[++i];
        config.strip_query_groups.clear();
        size_t start = 0;
        while (start < val.size()) {
          size_t comma = val.find(',', start);
          if (comma == std::string::npos) comma = val.size();
          std::string token = val.substr(start, comma - start);
          if (!token.empty()) config.strip_query_groups.push_back(token);
          start = comma + 1;
        }
      } else if (arg == "--strip-query-params" && i + 1 < argc) {
        std::string val = argv[++i];
        config.strip_query_params.clear();
        size_t start = 0;
        while (start < val.size()) {
          size_t comma = val.find(',', start);
          if (comma == std::string::npos) comma = val.size();
          std::string token = val.substr(start, comma - start);
          if (!token.empty()) config.strip_query_params.push_back(token);
          start = comma + 1;
        }
      } else if (arg == "--host-alias" && i + 1 < argc) {
        std::string val = argv[++i];
        size_t eq = val.find('=');
        if (eq == std::string::npos || eq == 0 || eq + 1 >= val.size()) {
          std::cerr << "Error: --host-alias requires src=dst format\n";
          return 1;
        }
        config.host_aliases.emplace_back(val.substr(0, eq), val.substr(eq + 1));
      } else if (arg == "--allow-private-urls") {
        config.allow_private_urls = true;
      } else if (arg == "--version") {
        std::cout << "mod_pagespeed " << pagespeed::kPageSpeedVersion << " ("
                  << pagespeed::kBuildCommitShort << ")\n";
        return 0;
      } else if (arg == "--help") {
        PrintUsage(argv[0]);
        return 0;
      } else {
        std::cerr << "Unknown option: " << arg << "\n";
        PrintUsage(argv[0]);
        return 1;
      }
    }
  } catch (const std::invalid_argument& e) {
    std::cerr << "Invalid argument: " << e.what() << "\n";
    PrintUsage(argv[0]);
    return 1;
  } catch (const std::out_of_range& e) {
    std::cerr << "Value out of range: " << e.what() << "\n";
    PrintUsage(argv[0]);
    return 1;
  }

  // Final cache-path resolution (normally already settled in pass 1):
  // --cache-path — CLI or persisted config file — is the expert override
  // and wins; otherwise the volume stem derives from --cache-dir, falling
  // back to the compiled-in generationed default directory.
  if (config.cache_path.empty()) {
    config.cache_path = CachePathFromDir(
        cache_dir_arg.empty() ? DefaultCacheDir() : cache_dir_arg);
  }

  // Loud, unconditional, and before anything else is logged: this switch
  // suppresses a FOUC safety gate. std::cerr rather than the message handler
  // so no --log-level can hide it.
  if (config.unsafe_force_async_css) {
    std::cerr << pagespeed::kUnsafeForceAsyncCssWarning << "\n";
  }

  // Create message handler based on format
  std::unique_ptr<pagespeed::MessageHandler> handler;
  if (log_format_str == "json") {
    handler = std::make_unique<pagespeed::JsonMessageHandler>();
  } else if (log_format_str == "text") {
    handler = std::make_unique<pagespeed::ConsoleMessageHandler>();
  } else {
    std::cerr << "Unknown log format: " << log_format_str << "\n";
    PrintUsage(argv[0]);
    return 1;
  }

  // Apply log level setting
  if (log_level_str == "debug") {
    handler->SetMinLogLevel(pagespeed::LogLevel::kDebug);
  } else if (log_level_str == "info") {
    handler->SetMinLogLevel(pagespeed::LogLevel::kInfo);
  } else if (log_level_str == "warning") {
    handler->SetMinLogLevel(pagespeed::LogLevel::kWarning);
  } else if (log_level_str == "error") {
    handler->SetMinLogLevel(pagespeed::LogLevel::kError);
  } else {
    std::cerr << "Unknown log level: " << log_level_str << "\n";
    PrintUsage(argv[0]);
    return 1;
  }

  // API token: command-line flag overrides env var
  if (config.api_token.empty()) {
    const char* env_token = std::getenv("PAGESPEED_API_TOKEN");
    if (env_token != nullptr) {
      config.api_token = env_token;
    }
  }

  // Trim the token, whichever way it arrived.  An EnvironmentFile line, a
  // secret mounted as a file, and a shell heredoc all routinely carry a
  // trailing newline or stray spaces, and a token that differs from the one
  // the operator pasted by one invisible byte fails every request with 403 --
  // a debugging session that teaches nothing.  Compare what they meant.
  {
    constexpr std::string_view kSpace = " \t\r\n\f\v";
    const size_t first = config.api_token.find_first_not_of(kSpace);
    if (first == std::string::npos) {
      config.api_token.clear();
    } else {
      const size_t last = config.api_token.find_last_not_of(kSpace);
      config.api_token = config.api_token.substr(first, last - first + 1);
    }
    // A short token is not a weak credential, it is a typo or a truncated
    // secret mount -- and silently accepting it would put a guessable
    // credential in front of cache purge.  Treat it as absent (so the
    // invariant below refuses, or --api-no-auth applies) and say why.
    constexpr size_t kMinApiTokenChars = 16;
    if (!config.api_token.empty() &&
        config.api_token.size() < kMinApiTokenChars) {
      std::cerr << "Error: the management API token is only "
                << config.api_token.size() << " characters; at least "
                << kMinApiTokenChars
                << " are required. Treating it as unset -- check "
                   "PAGESPEED_API_TOKEN for a truncated or mis-pasted value "
                   "(the packaged installer generates a 43-character one into "
                   "/etc/pagespeed-optimizer/daemon.env).\n";
      config.api_token.clear();
    }
  }

  // API unix socket: env fallback for the flag.  Any non-empty value enables
  // the transport; the literal "1"/"true"/"default" selects the packaged path.
  if (config.api_socket_path.empty()) {
    const char* env_socket = std::getenv("PAGESPEED_API_SOCKET");
    if (env_socket != nullptr && env_socket[0] != '\0') {
      const std::string_view value(env_socket);
      // An operator writing PAGESPEED_API_SOCKET=false means "off", not "put
      // the socket at ./false".  Honour the boolean spellings in BOTH
      // directions; anything else is taken as a path.
      if (value == "0" || value == "false" || value == "off" || value == "no") {
        config.api_socket_path.clear();
      } else if (value == "1" || value == "true" || value == "default") {
        config.api_socket_path = pagespeed::kDefaultApiSocketPath;
      } else {
        config.api_socket_path = std::string(value);
      }
    }
  }

  // The two deliberate opt-outs, env fallbacks for the flags.
  if (!config.api_allow_remote) {
    const char* env_remote = std::getenv("PAGESPEED_API_ALLOW_REMOTE");
    if (env_remote != nullptr && std::string_view(env_remote) == "true") {
      config.api_allow_remote = true;
    }
  }
  if (!config.api_no_auth) {
    const char* env_no_auth = std::getenv("PAGESPEED_API_NO_AUTH");
    if (env_no_auth != nullptr && std::string_view(env_no_auth) == "true") {
      config.api_no_auth = true;
    }
  }

  // Both of these live in free functions rather than inline blocks: main()
  // is already deeply nested, and MSVC's block-nesting limit is a real one
  // (C1061).  Each returns false to mean "refuse to start"; each has already
  // printed the reason.
  if (!ResolveBrowserSandboxMode(config)) {
    return 1;
  }
  if (!ValidateManagementApiConfig(config)) {
    return 1;
  }

  // API read-open: env var as fallback for CLI flag
  if (!config.api_read_open) {
    const char* env_read_open = std::getenv("PAGESPEED_API_READ_OPEN");
    if (env_read_open != nullptr && std::string_view(env_read_open) == "true") {
      config.api_read_open = true;
    }
  }

  // Security headers: env var as fallback for CLI flag
  if (config.security_headers) {
    const char* env_sec = std::getenv("PAGESPEED_SECURITY_HEADERS");
    if (env_sec != nullptr && std::string_view(env_sec) == "false") {
      config.security_headers = false;
    }
  }

  // Retired 2.0 licensing environment.  An unchanged 2.0 env file
  // may still set these; say once what changed and ignore them.  The values
  // are never read.
  for (const char* env_var :
       {"PAGESPEED_LICENSE_KEY", "PAGESPEED_LICENSE_RENEWAL_URL"}) {
    if (std::getenv(env_var) != nullptr) {
      WarnRetiredLicensingSetting(env_var);
    }
  }

  // Resolve auto thread count.
  // Precedence: explicit --num-threads, then CPU affinity, then the container
  // CPU quota (cgroup v2, then v1), then the hardware thread count.
  const char* thread_source = "--num-threads";
  if (config.num_threads == 0) {
    pagespeed::CpuBudget budget = pagespeed::AvailableCpus();
    config.num_threads = pagespeed::DefaultThreadCount(budget);
    thread_source = budget.source;
  }
  handler->Info("Thread pool size: %d (from %s)", config.num_threads,
                thread_source);

  // Set libuv thread pool size before any uv_queue_work calls.
  // UV_THREADPOOL_SIZE must be set before the first libuv loop starts.
#ifdef _WIN32
  _putenv_s("UV_THREADPOOL_SIZE", std::to_string(config.num_threads).c_str());
#else
  setenv("UV_THREADPOOL_SIZE", std::to_string(config.num_threads).c_str(), 1);
#endif

  // Create worker
  pagespeed::Worker worker(config, handler.get());
  g_worker = &worker;

  // Set up signal handlers
  std::signal(SIGINT, SignalHandler);
  std::signal(SIGTERM, SignalHandler);
  // As a Windows service, the manager's Stop is the same request.
  pagespeed::ServiceSetStopHandler([] { SignalHandler(SIGTERM); });
#ifndef _WIN32
  // Ignore SIGHUP rather than dying from it (#1465).  The daemon's settings
  // are fixed at startup (cache location, socket paths, thread pool), so
  // there is no reload semantics for SIGHUP to trigger -- hot-reloadable
  // settings go through PATCH /v1/config.  Left unhandled, SIGHUP's default
  // disposition terminates the process, which turned `systemctl reload`
  // (SIGHUP when the unit declares no ExecReload=) and any stray SIGHUP into
  // an unannounced restart.  SIG_IGN survives execve, so the curl helper
  // inherits it (curl does not use SIGHUP); the browser does not, because
  // it is spawned through libuv, which resets every disposition in the child.
  std::signal(SIGHUP, SIG_IGN);
#endif

  // Initialize worker
  if (!worker.Initialize()) {
    std::cerr << "Failed to initialize worker\n";
    return 1;
  }

  pagespeed::ServiceReportRunning();

  // Run worker
  handler->Info("mod_pagespeed %s (%s) optimizer worker starting",
                pagespeed::kPageSpeedVersion, pagespeed::kBuildCommitShort);
  worker.Run();

  // Prevent signal handler from dereferencing a dangling pointer
  // during ~Worker() if a second signal arrives.
  g_worker = nullptr;

  handler->Info("mod_pagespeed %s (%s) optimizer worker stopped",
                pagespeed::kPageSpeedVersion, pagespeed::kBuildCommitShort);

  return 0;
}

int main(int argc, char* argv[]) {  // NOLINT(bugprone-exception-escape)
  int exit_code = 0;
  if (pagespeed::RunAsServiceIfRequested(argc, argv, &WorkerMain, &exit_code)) {
    return exit_code;
  }
  return WorkerMain(argc, argv);
}
