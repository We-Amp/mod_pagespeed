// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - Shared Config File (worker→nginx IPC)

#include "src/worker/shared_config.h"

#include <atomic>
#include <cerrno>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include "lib/base/atomic_file_writer.h"
#include "lib/base/message_handler.h"
#include "lib/base/posix_compat.h"

namespace pagespeed {
namespace {

// Sticky process-wide record of the version-mismatch condition, plus the
// handler the signal goes to.  Held in function-local statics: this file is
// linked into the worker, into the embedding C API library
// (//lib/pagespeed:pagespeed_api) and into tests, and namespace-scope statics
// would make the record's initialization order depend on all of them.
struct VersionSkewState {
  std::mutex mu;
  SharedConfigVersionSkew skew;
  // Last version already reported.  A mismatched file is re-read on every
  // config reload and, through the embedding C API, potentially far more
  // often than that; reporting each parse would turn the one loud signal into
  // noise an operator learns to ignore.  Reporting each DISTINCT version keeps
  // it loud and keeps a second, differently-skewed peer from being swallowed.
  int reported_version = 0;
  bool reported = false;
};

VersionSkewState& SkewState() {
  static VersionSkewState state;
  return state;
}

std::atomic<MessageHandler*>& HandlerSlot() {
  static std::atomic<MessageHandler*> slot{nullptr};
  return slot;
}

// The configured handler, or the stderr default.
MessageHandler& SkewHandler() {
  MessageHandler* configured = HandlerSlot().load(std::memory_order_acquire);
  if (configured != nullptr) {
    return *configured;
  }
  static ConsoleMessageHandler console;
  return console;
}

// Record a version this build cannot read, and report it if it is new.
void RecordVersionSkew(int observed_version) {
  bool report = false;
  {
    auto& state = SkewState();
    std::lock_guard<std::mutex> lock(state.mu);
    state.skew.mismatch = true;
    state.skew.observed_version = observed_version;
    state.skew.supported_version = kSharedConfigVersion;
    ++state.skew.observations;
    if (!state.reported || state.reported_version != observed_version) {
      state.reported = true;
      state.reported_version = observed_version;
      report = true;
    }
  }
  if (!report) {
    return;
  }
  // Names BOTH versions, says what was done instead, and says what it costs.
  // An operator reading this must not have to infer any of the three.
  SkewHandler().Error(
      "pagespeed: shared config declares schema version %d, but this build "
      "reads version %d. The file was NOT parsed: every setting it carries "
      "(socket path and the serve-side toggles) is now the compiled-in "
      "default, not what the file says. Optimization continues on "
      "defaults; align the versions of the components sharing this cache "
      "directory to restore the configured behaviour.",
      observed_version, kSharedConfigVersion);
}

}  // namespace

SharedConfigVersionSkew SharedConfigVersionSkewState() {
  auto& state = SkewState();
  std::lock_guard<std::mutex> lock(state.mu);
  return state.skew;
}

void ResetSharedConfigVersionSkewForTesting() {
  auto& state = SkewState();
  std::lock_guard<std::mutex> lock(state.mu);
  state.skew = SharedConfigVersionSkew{};
  state.reported = false;
  state.reported_version = 0;
}

void SetSharedConfigMessageHandler(MessageHandler* handler) {
  HandlerSlot().store(handler, std::memory_order_release);
}

std::string SharedConfigFilePath(const std::string& cache_path) {
  if (cache_path.empty()) {
    return {};
  }
  std::filesystem::path p(cache_path);
  return (p.parent_path() / "pagespeed-shared.conf").string();
}

SharedConfig ParseSharedConfig(std::string_view content) {
  SharedConfig config;
  int version = -1;
  // Resolved after the loop, so html_mode wins over disable_html whatever
  // order the lines come in.
  std::optional<HtmlMode> html_mode;
  std::optional<bool> disable_html;

  size_t pos = 0;
  while (pos < content.size()) {
    // Find end of line.
    size_t eol = content.find('\n', pos);
    if (eol == std::string_view::npos) {
      eol = content.size();
    }
    std::string_view line = content.substr(pos, eol - pos);
    pos = eol + 1;

    // Strip trailing \r (Windows line endings, git normalization).
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }

    // Skip empty lines and comments.
    if (line.empty() || line[0] == '#') {
      continue;
    }

    // Find first '=' — values may contain '=' (e.g., base64 padding).
    size_t eq = line.find('=');
    if (eq == std::string_view::npos) {
      continue;
    }

    std::string_view key = line.substr(0, eq);
    std::string_view value = line.substr(eq + 1);

    // Reject values with null bytes — downstream code using c_str() would
    // silently truncate, creating path confusion or injection.
    if (value.find('\0') != std::string_view::npos) {
      continue;
    }

    if (key == "version") {
      int v = 0;
      auto [ptr, ec] =
          std::from_chars(value.data(), value.data() + value.size(), v);
      if (ec == std::errc{}) {
        version = v;
      }
      if (version > kSharedConfigVersion) {
        // A schema this build cannot read.  Pass through on compiled-in
        // defaults rather than best-effort parsing a newer peer's file, and
        // say so out loud — this is the one skew condition that is detectable
        // at all, so it is the one that has to be heard.  Anything parsed
        // before the version line is discarded with the rest.
        RecordVersionSkew(version);
        return SharedConfig{};
      }
    } else if (key == "pid") {
      // Informational only; not stored in SharedConfig.
    } else if (key == "socket_path") {
      // Skip empty values — keep default rather than setting empty path.
      // Unix socket paths have a hard 108-byte limit (sockaddr_un.sun_path).
      if (!value.empty() && value.size() <= 107) {
        config.socket_path = std::string(value);
      }
    } else if (key == "disable_html") {
      disable_html = (value == "true" || value == "1");
    } else if (key == "html_mode") {
      // An unknown value (say, one a newer writer added) is ignored, so the
      // disable_html that writer also emitted decides instead.
      if (auto mode = ParseHtmlMode(value)) {
        html_mode = mode;
      }
    } else if (key == "agent_optimize_entitled") {
      config.agent_optimize_entitled = (value == "true" || value == "1");
    } else if (key == "agent_optimize_llms_txt_enabled") {
      config.agent_optimize_llms_txt_enabled =
          (value == "true" || value == "1");
    } else if (key == "web_bot_auth") {
      config.web_bot_auth = (value == "true" || value == "1");
    } else if (key == "web_bot_auth_verified_bots") {
      config.web_bot_auth_verified_bots = std::string(value);
    } else if (key == "web_bot_auth_directory_hosts") {
      config.web_bot_auth_directory_hosts = std::string(value);
    } else if (key == "web_bot_auth_public_counter") {
      // Validate against the known modes; anything else keeps the default
      // ("off"), same idiom as cache_mode.
      if (value == "off" || value == "private" || value == "public") {
        config.web_bot_auth_public_counter = std::string(value);
      }
    } else if (key == "rsl_cap_enforcement") {
      config.rsl_cap_enforcement = (value == "true" || value == "1");
    } else if (key == "rsl_cap_directory_hosts") {
      config.rsl_cap_directory_hosts = std::string(value);
    } else if (key == "rsl_cap_requested_license") {
      config.rsl_cap_requested_license = std::string(value);
    } else if (key == "rsl_cap_requested_scope") {
      config.rsl_cap_requested_scope = std::string(value);
    } else if (key == "rsl_cap_issuer") {
      config.rsl_cap_issuer = std::string(value);
    } else if (key == "volume_size") {
      uint64_t v = 0;
      auto [ptr, ec] =
          std::from_chars(value.data(), value.data() + value.size(), v);
      // Anything unparseable leaves the field at 0, which reads as "not
      // stated" — the same as an older writer that never emitted the key.
      // Consumers already have to handle that, so a malformed value degrades
      // into an existing, safe state rather than into a wrong number.
      if (ec == std::errc{} && ptr == value.data() + value.size()) {
        config.volume_size = v;
      }
    } else if (key == "cache_dir_generation") {
      int v = 0;
      auto [ptr, ec] =
          std::from_chars(value.data(), value.data() + value.size(), v);
      // Unparseable leaves 0, "not stated" — same as an older writer that
      // never emitted the key (same degrade idiom as volume_size).
      if (ec == std::errc{} && ptr == value.data() + value.size() && v > 0) {
        config.cache_dir_generation = v;
      }
    } else if (key == "cache_mode") {
      if (value == "safe" || value == "aggressive") {
        config.cache_mode = std::string(value);
      }
    } else if (key == "strip_query_extensions") {
      config.strip_query_extensions = std::string(value);
    } else if (key == "strip_query_params") {
      config.strip_query_params = std::string(value);
    }
    // Unknown keys silently ignored.
  }

  config.html_mode = ResolveHtmlMode(html_mode, disable_html);
  return config;
}

// Maximum shared config file size (64 KB).
static constexpr size_t kMaxSharedConfigSize = size_t{64} * 1024;

SharedConfig ReadSharedConfigFile(const std::string& path) {
  if (path.empty()) {
    return SharedConfig{};
  }

  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f.is_open()) {
    return SharedConfig{};
  }

  auto size = f.tellg();
  if (size < 0 || static_cast<size_t>(size) > kMaxSharedConfigSize) {
    return SharedConfig{};
  }

  f.seekg(0, std::ios::beg);
  std::string content(static_cast<size_t>(size), '\0');
  if (!f.read(content.data(), size)) {
    return SharedConfig{};
  }

  return ParseSharedConfig(content);
}

bool WriteSharedConfigFile(const std::string& path, const SharedConfig& config,
                           pid_t pid) {
  if (path.empty()) {
    return false;
  }

  if (pid == 0) {
#ifdef _WIN32
    pid = _getpid();
#else
    pid = ::getpid();
#endif
  }

  // Reject values that could inject extra config lines.
  auto has_newline = [](std::string_view s) {
    return s.find('\n') != std::string_view::npos ||
           s.find('\r') != std::string_view::npos;
  };
  if (has_newline(config.socket_path) || has_newline(config.cache_mode) ||
      has_newline(config.strip_query_extensions) ||
      has_newline(config.strip_query_params) ||
      has_newline(config.web_bot_auth_verified_bots) ||
      has_newline(config.web_bot_auth_directory_hosts) ||
      has_newline(config.web_bot_auth_public_counter) ||
      has_newline(config.rsl_cap_directory_hosts) ||
      has_newline(config.rsl_cap_requested_license) ||
      has_newline(config.rsl_cap_requested_scope) ||
      has_newline(config.rsl_cap_issuer)) {
    return false;
  }

  std::string content;
  content += "# Written by pagespeed-worker. Do not edit.\n";
  content += "version=" + std::to_string(kSharedConfigVersion) + "\n";
  content += "pid=" + std::to_string(pid) + "\n";
  content += "socket_path=" + config.socket_path + "\n";
  // license_key / license_valid / license_checked_once are deliberately NOT
  // written since 2.1: there is no license state.  A 2.0-era
  // reader keeps its defaults for the missing keys (see kSharedConfigVersion).
  // html_mode, plus the disable_html an older reader understands.  Those
  // readers ignore the unknown html_mode key and see every markdown-* mode as
  // full, which is what they did before the mode existed.
  content += "html_mode=" + std::string(HtmlModeName(config.html_mode)) + "\n";
  content +=
      "disable_html=" +
      std::string(HtmlModeDisablesHtml(config.html_mode) ? "true" : "false") +
      "\n";
  content += "agent_optimize_entitled=" +
             std::string(config.agent_optimize_entitled ? "true" : "false") +
             "\n";
  content +=
      "agent_optimize_llms_txt_enabled=" +
      std::string(config.agent_optimize_llms_txt_enabled ? "true" : "false") +
      "\n";
  content +=
      "web_bot_auth=" + std::string(config.web_bot_auth ? "true" : "false") +
      "\n";
  content +=
      "web_bot_auth_verified_bots=" + config.web_bot_auth_verified_bots + "\n";
  content +=
      "web_bot_auth_directory_hosts=" + config.web_bot_auth_directory_hosts +
      "\n";
  content +=
      "web_bot_auth_public_counter=" + config.web_bot_auth_public_counter +
      "\n";
  content += "rsl_cap_enforcement=" +
             std::string(config.rsl_cap_enforcement ? "true" : "false") + "\n";
  content += "rsl_cap_directory_hosts=" + config.rsl_cap_directory_hosts + "\n";
  content +=
      "rsl_cap_requested_license=" + config.rsl_cap_requested_license + "\n";
  content += "rsl_cap_requested_scope=" + config.rsl_cap_requested_scope + "\n";
  content += "rsl_cap_issuer=" + config.rsl_cap_issuer + "\n";
  // Emitted only when known.  Writing volume_size=0 would say "the volume is
  // zero bytes" to a reader that has no way to tell that apart from "the
  // writer did not know", and the two have to stay distinguishable — see the
  // field comment in shared_config.h.
  if (config.volume_size != 0) {
    content += "volume_size=" + std::to_string(config.volume_size) + "\n";
  }
  // Emitted only when stated (0 = a writer that does not know its
  // generation).  A reader MUST be able to tell "old writer" apart from
  // "generation N" — emitting 0 would erase that distinction.
  if (config.cache_dir_generation > 0) {
    content +=
        "cache_dir_generation=" + std::to_string(config.cache_dir_generation) +
        "\n";
  }
  if (!config.cache_mode.empty()) {
    content += "cache_mode=" + config.cache_mode + "\n";
  }
  content += "strip_query_extensions=" + config.strip_query_extensions + "\n";
  content += "strip_query_params=" + config.strip_query_params + "\n";

  // 0640: owner (daemon) rw, group (the cache-sharing peers: web-server
  // workers in group `pagespeed`) r, others nothing.  Was 0644 until the
  // H1-H3 privilege drop; group-read suffices for every legitimate peer, and
  // the mode is kept narrow even though the file carries no secret any more.
  const bool ok = AtomicWriteFile(path, content, 0640);
  const int saved_errno = errno;
  content.clear();
  content.shrink_to_fit();
  errno = saved_errno;
  return ok;
}

}  // namespace pagespeed
