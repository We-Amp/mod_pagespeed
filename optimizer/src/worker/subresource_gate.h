// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef SRC_WORKER_SUBRESOURCE_GATE_H_
#define SRC_WORKER_SUBRESOURCE_GATE_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "lib/net/upstream_pin.h"
#include "src/browser/agent_fetcher.h"

// Whether a cached sub-resource may be used to build another URL's output.
//
// Outputs built from other URLs (a page's combined or inlined stylesheets,
// flattened @imports, image dimensions, /llms.txt summaries) are served to
// every client of the output, so they are built only from content the site
// serves to any client.  Before the worker uses another URL's cached bytes,
// it asks the site once with a HEAD request from its own fetcher (no
// cookies or credentials).  Only a 200 that is not marked private or
// no-store and sets no cookie admits the URL; anything else, or no answer,
// counts as not admitted, and the caller treats the resource as absent.
//
// Answers are remembered per (site, scheme, host, URL) for a short time, so
// a page whose sub-resources are all public costs one request per
// sub-resource per memo period.  Reads of the notified URL's own bytes are
// not gated.
//
// For a page whose host the worker may not ask, a stylesheet or image the
// front end recorded from its response to an anonymous request
// (AlternateMetadata::kFlagAnonymousRecorded, carried by every variant the
// worker derives from it) is admitted without a request when it was read from
// the page's own site keyspace and its recorded Cache-Control is neither
// private nor no-store.  Where the worker may ask the host, a stamped
// entry is asked about like any other.

namespace pagespeed {

enum class SubresourceVerdict : std::uint8_t {
  kAdmitted,  // the server answered an anonymous HEAD with 200
  kRefused,   // the server answered with anything else
  kUnknown,   // no answer, or the worker may not ask (see SubresourceRead)
};

// One cached read the worker is about to make for another URL's output.  The
// fields are the read's own key: `url` is the cache URL (path and query,
// leading '/'), `host` the host it is keyed under.
struct SubresourceRead {
  std::string_view site;
  std::string_view scheme;  // empty reads as "https"
  std::string_view host;
  std::string_view url;
  // The host the notification named (the page's host).  The worker asks only
  // that host, and only when the sender may ask for fetches from it (it
  // vouched for the host and carries no options context); `host_fetch_allowed`
  // carries that rule.  A read under any other host is never asked about.
  std::string_view page_host;
  bool host_fetch_allowed = false;
  // The cache entry the caller read for this URL, when it read one: its flag
  // bits and the site keyspace it was read from.  When `host_fetch_allowed`
  // is false, an entry stamped kFlagAnonymousRecorded and read from `site` is
  // admitted without asking.
  bool entry_read = false;
  std::uint8_t entry_flags = 0;
  std::string_view entry_site;
  // The entry's origin_cc_flags.  A stamped entry is admitted without asking
  // only when these carry neither no-store nor private.
  std::uint16_t entry_cc_flags = 0;
  // The read feeds a copy served to other clients (the agent render's
  // Markdown copy, /llms.txt).  The server is then asked with the
  // shared-copy HEAD (SubresourceGate::SetSharedCopyHead), whose answer is
  // refused when marked private or no-store or when the host resolves to a
  // loopback or private address not listed in `private_address_origins`.
  // Remembered apart from ordinary reads.
  bool shared_copy = false;
  const std::vector<PinnedUpstream>* private_address_origins = nullptr;
};

class SubresourceGate {
 public:
  // Sends one anonymous HEAD for `absolute_url` to `origin`
  // ("scheme://host[:port]") and returns the response status, or nullopt when
  // there was no response (policy refusal, transport failure, redirect).
  using HeadFn = std::function<std::optional<int>(
      const std::string& origin, const std::string& absolute_url)>;
  // The HEAD for shared-copy reads: as HeadFn, plus the operator's
  // private-address origins (AgentFetcherDeps::private_address_origins).
  using SharedCopyHeadFn = std::function<std::optional<int>(
      const std::string& origin, const std::string& absolute_url,
      const std::vector<PinnedUpstream>& private_address_origins)>;
  // Monotonic milliseconds.
  using NowFn = std::function<std::int64_t()>;

  struct Options {
    std::int64_t admitted_ttl_ms = 300 * 1000;
    std::int64_t refused_ttl_ms = 60 * 1000;
    std::size_t max_entries = 8192;
  };

  SubresourceGate(HeadFn head, NowFn now, Options options);
  SubresourceGate(HeadFn head, NowFn now)
      : SubresourceGate(std::move(head), std::move(now), Options()) {}

  // The verdict for `read`.  `may_fetch` false answers from the memo only (for
  // callers on the event loop, which must not block on a request); a memo
  // miss is then kUnknown.
  SubresourceVerdict Check(const SubresourceRead& read, bool may_fetch);

  bool Admitted(const SubresourceRead& read, bool may_fetch) {
    return Check(read, may_fetch) == SubresourceVerdict::kAdmitted;
  }

  // Replaces the HEAD used for ordinary reads.
  void SetHead(HeadFn head);

  // Sets the HEAD used for shared-copy reads.  Without one, a shared-copy
  // read is never admitted by asking (kUnknown).
  void SetSharedCopyHead(SharedCopyHeadFn head);

  std::uint64_t heads_sent() const;
  std::uint64_t refusals() const;

  // Test seam for suites that exercise the worker's pipeline without a front
  // end to ask: while set, every gate in the process returns the hook's
  // verdict when it returns one, before any memo or request.  Not used in
  // production.
  using VerdictOverride =
      std::function<std::optional<SubresourceVerdict>(const SubresourceRead&)>;
  static void SetVerdictOverrideForTest(VerdictOverride hook);

 private:
  struct Entry {
    SubresourceVerdict verdict;
    std::int64_t expires_at_ms;
  };

  HeadFn head_;                        // guarded by mutex_
  SharedCopyHeadFn shared_copy_head_;  // guarded by mutex_
  NowFn now_;
  Options options_;
  mutable std::mutex mutex_;
  std::unordered_map<std::string, Entry> memo_;
  std::uint64_t heads_sent_ = 0;
  std::uint64_t refusals_ = 0;
};

// The production HEAD: the worker's pinned curl fetcher (anonymous, no
// cookies or credentials, the worker's fixed User-Agent), pinned to `origin`,
// redirects not followed, a short timeout.  Blocking.
// `spawn` / `resolve` replace the real curl and resolver (tests only).
// `log_refusal`, when set, receives a message the first time an origin's
// answer is refused as not shareable, naming the reason.  A refused answer
// reads as kRefusedStatus (a refusal, not "no answer").
SubresourceGate::HeadFn MakeAnonymousHeadFn(
    SpawnCurlFn spawn = nullptr, ResolveHostFn resolve = nullptr,
    std::function<void(const std::string&)> log_refusal = nullptr);

// The status a HEAD reports for an answer refused as not shareable.
inline constexpr int kRefusedStatus = 0;

// Whether an entry's recorded origin Cache-Control (origin_cc_flags) allows
// its bytes in a shared output: neither no-store nor private.
bool CcFlagsAllowShared(std::uint16_t origin_cc_flags);

// The production shared-copy HEAD: MakeAnonymousHeadFn with the agent
// render's shared-copy refusals (AgentFetcherDeps::shared_copy).
// `log_refusal`, when set, receives a message the first time an origin is
// refused for resolving to a loopback or private address.
SubresourceGate::SharedCopyHeadFn MakeSharedCopyHeadFn(
    std::function<void(const std::string&)> log_refusal = nullptr);

// A monotonic clock in milliseconds.
SubresourceGate::NowFn MonotonicNowMs();

}  // namespace pagespeed

#endif  // SRC_WORKER_SUBRESOURCE_GATE_H_
