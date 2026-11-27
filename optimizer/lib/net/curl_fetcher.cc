// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "lib/net/curl_fetcher.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace pagespeed {

std::vector<std::string> BuildPinnedGetArgv(std::string_view url,
                                            std::string_view host, int port,
                                            std::string_view pinned_ip,
                                            const CurlFetchOptions& opts) {
  // Fail closed: never emit a command if any pin input is missing/invalid.
  if (url.empty() || host.empty() || pinned_ip.empty()) return {};
  if (port <= 0 || port > 65535) return {};

  // Fail closed on any byte that the policy's URL parser (ssrf_guard ParseHttpUrl)
  // normalizes or strips but curl does NOT — backslash (WHATWG `\`->`/`), tab, LF,
  // CR. The security decision (G1/G2/SSRF) ran on the parser's normalized host; if
  // such a byte survives into the URL handed to curl, curl can parse a DIFFERENT
  // authority (e.g. `host\@127.0.0.1` => curl host 127.0.0.1) and the --resolve IP
  // pin is silently bypassed => SSRF. A conformant URL never contains these raw
  // (they are percent-encoded).
  for (char c : url) {
    if (c == '\\' || c == '\t' || c == '\n' || c == '\r') return {};
  }

  std::vector<std::string> argv = {
      "curl",
      "-q",              // MUST be first: ignore ~/.curlrc / $CURL_HOME
                         // (a planted config can inject proxy=/insecure)
      "-sS",             // silent, but report transport errors
      "-g",              // --globoff: never treat {}[] in the URL as a
                         // glob (a glob rewrites the host past --resolve
                         // into an un-pinned system-DNS resolution)
      "--no-netrc",      // never read ~/.netrc credentials (§3.4)
      "--noproxy", "*",  // ignore http_proxy/https_proxy/all_proxy (env
                         // OR curlrc): a proxy tunnels the request past
                         // --resolve to an un-pinned host => SSRF. The
                         // fetcher ALSO scrubs *_proxy from the spawn env
                         // (defense in depth); this makes the command
                         // self-protecting regardless of environment.
      "--proto", "=http,https",        // scheme allowlist
      "--proto-redir", "=http,https",  // and on any redirect
      "--max-redirs", "0",  // never auto-follow; the fetcher re-adjudicates
      "--max-time", std::to_string(opts.max_time_sec), "-A",
      opts.user_agent,  // fixed, anonymous UA (no page UA forwarded)
      "-H", "Accept: " + opts.accept,  // the only content header we send
                                       // besides the encoding negotiation
      "--compressed",  // ask for gzip/br and let curl decode it. The size cap
                       // that matters is the one on curl's stdout, which the
                       // spawner enforces on the DECODED bytes (--max-filesize
                       // only bounds the encoded transfer).
  };

  // A GET's encoded transfer is capped. A HEAD has no body to cap, and curl
  // checks --max-filesize against the advertised Content-Length even for a
  // HEAD, so a cap would refuse the answer for any resource larger than it.
  // A HEAD's output (status line and headers) stays bounded by the spawner's
  // limit on curl's stdout.
  if (!opts.head) {
    argv.emplace_back("--max-filesize");
    argv.emplace_back(std::to_string(opts.max_response_bytes));
  }

  // Pin the connection to the validated IP. When the host is already an IP literal
  // (host == pinned_ip) the URL itself pins the target and --resolve is redundant.
  if (host != pinned_ip) {
    // curl's --resolve IPv6 address form is bracketed: host:port:[addr].
    const std::string addr = (pinned_ip.find(':') != std::string_view::npos)
                                 ? "[" + std::string(pinned_ip) + "]"
                                 : std::string(pinned_ip);
    argv.emplace_back("--resolve");
    argv.emplace_back(std::string(host) + ":" + std::to_string(port) + ":" +
                      addr);
  }

  argv.emplace_back("-i");  // include response headers (status/Location/CT)
  if (opts.head) {
    argv.emplace_back("--head");  // HEAD: status and headers, no body
  } else {
    argv.emplace_back("-X");
    argv.emplace_back("GET");  // GET only — no body, no other method
  }
  argv.emplace_back("--url");  // explicit URL value — a '-'-leading URL can
  argv.emplace_back(std::string(url));  // never be parsed as a curl flag
  return argv;
}

}  // namespace pagespeed
