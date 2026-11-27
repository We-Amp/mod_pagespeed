// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// The seam between the shared admin purge (AdminSite::Purge) and the HTML
// cache engine, on its own so the system library can name it without
// depending on the engine.
//
// An admin purge is one of two scopes: a URL, or everything (a URL ending in
// "*").  The purge set itself is written by AdminSite::Purge, through the
// cache path's purge context; an observer adds what the purge set cannot
// reach: the entry the HTML cache stored for the URL in the optimizer's
// volume, or the per-site purge epoch beside that volume.  The admin response
// reports success only when the purge set AND every observer succeeded, so a
// purge an operator was told worked has reached both.

#ifndef PAGESPEED_SYSTEM_DAEMON_HTML_PURGE_OBSERVER_H_
#define PAGESPEED_SYSTEM_DAEMON_HTML_PURGE_OBSERVER_H_

#include <functional>
#include <vector>

#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"

namespace net_instaweb {

class DaemonHtmlPurgeObserver {
 public:
  // Called once, with ok false and a short reason when the observer could
  // not carry the purge out.  May be called on another thread, and before
  // OnAdminPurge returns.
  using Done = std::function<void(bool ok, const GoogleString& reason)>;

  virtual ~DaemonHtmlPurgeObserver() = default;

  // `url` is the purge as the admin gave it (for a global purge, the URL
  // ending in "*"); `now_ms` the time the purge set records for it.
  virtual void OnAdminPurge(StringPiece url, bool global, int64 now_ms,
                            Done done) = 0;
};

// The keys an admin purge of `url` adds to the purge set: the URL as given,
// and, when it differs, its normalised form -- the host lowercased and a
// default port (":80" for http, ":443" for https) removed, which is the form
// the HTML cache checks a stored entry under.  The normalised form is added
// only when it differs, so an already-normal URL takes one purge-set entry,
// not two.  A URL that does not start with "http://" or "https://" yields
// only itself.
std::vector<GoogleString> DaemonHtmlPurgeKeys(StringPiece url);

// The normalised form alone (see above); `url` itself when it has no
// http or https scheme.
GoogleString DaemonHtmlNormalizePurgeUrl(StringPiece url);

}  // namespace net_instaweb

#endif  // PAGESPEED_SYSTEM_DAEMON_HTML_PURGE_OBSERVER_H_
