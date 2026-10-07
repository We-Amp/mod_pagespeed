/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#ifndef PAGESPEED_IIS_IIS_DAEMON_CHECK_H_
#define PAGESPEED_IIS_IIS_DAEMON_CHECK_H_

#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>

#include "pagespeed/system/daemon_adapter.h"
#include "pagespeed/system/daemon_health.h"

namespace net_instaweb {

class MessageHandler;

// The process-wide registry of daemon startup checks, one per distinct
// (library path, socket path, volume path) triple.
//
// A site's server context is rebuilt on every configuration edit, and during
// a rebuild an old and a new context can overlap, so this registry -- not any
// one context's lock -- serialises lookup-or-create.  Each entry owns ONE
// adapter and ONE verdict for the life of the process: the client library is
// never unloaded (the shared loader's own rule), so loading it once per
// process per distinct triple and reusing the adapter keeps the loader
// reference count and the handle count flat across rebuilds.
//
// A VERDICT LIVES UNTIL THE PROCESS ENDS.  A configuration edit that leaves
// the paths unchanged does not re-probe, and that holds for every verdict,
// the split included: a daemon that is not answering when the first site
// with this configuration starts in a worker process leaves in-place
// optimization off for those sites until the application pool recycles.
// That is this platform's equivalent of the fix-and-restart the other ports
// state (they need a web-server restart for the same).  Every verdict other
// than the split leaves the site serving normally (in-place optimization
// simply stays off).
class IisDaemonCheckRegistry {
 public:
	// The outcome of asking for a triple.  The adapter is borrowed: the
	// registry owns it for the life of the process.
	struct Result {
		DaemonAdapter* adapter = nullptr;  // null when never checked
		bool split = false;                // the one refusal verdict
	};

	// Returns the registry singleton (never null).
	static IisDaemonCheckRegistry* Get();

	// Looks up the entry for (library, socket, volume), creating and
	// checking it on first sight.  The adapter logs through the registry's
	// OWN process-lifetime handler, never through a site's handler: a
	// configuration edit deletes the site's handler while this adapter
	// stays, so a site registers its handler separately (below).
	Result GetOrCreate(const GoogleString& library_path,
	                   const GoogleString& socket_path,
	                   const GoogleString& volume_path);

	// Adds `handler` to the set of live site handlers the registry's own
	// handler forwards the adapters' messages to.  A context registers the
	// handler its factory owns when it runs the check and deregisters it in
	// its Shutdown, so a fan-out never reaches a deleted handler.
	void RegisterSiteHandler(MessageHandler* handler);
	void UnregisterSiteHandler(MessageHandler* handler);

	// Test seam: forget everything.  Tests only.
	void ResetForTesting();

	// Test seam: count how many times the library binder ran.
	int binder_calls() const { return binder_calls_; }

	// Test seam: replaces the library binder.
	void set_binder(DaemonAdapter::AbiLoader loader) {
		loader_ = std::move(loader);
	}

 private:
	IisDaemonCheckRegistry();

	// The live site handlers the adapters' messages are forwarded to, and
	// the forwarding handler the adapters log through; defined in the .cc.
	// Owned for the life of the process, like the registry itself.
	struct SiteHandlerPool;
	SiteHandlerPool* const pool_;

	struct Entry {
		std::unique_ptr<DaemonAdapter> adapter;
		bool split = false;
	};

	mutable std::mutex mu_;
	std::map<std::tuple<GoogleString, GoogleString, GoogleString>, Entry>
	    entries_;
	std::set<GoogleString> split_logged_;  // event-log dedup, per message
	DaemonAdapter::AbiLoader loader_ = &LoadDaemonAbi;
	int binder_calls_ = 0;
};

// The default optimizer-daemon client-library path for this process: the
// directory of this module's own binary, joined with the file name the
// shared loader expects.  On Windows the loader refuses any library path
// that is not absolute, so the bare file name is never a usable path there
// and the registry's key carries the resolved absolute path.
GoogleString DefaultDaemonLibraryPath();

// One site's ask: the gate and the registry in one call.  With BOTH daemon
// paths empty nothing runs at all -- the registry is not even reached (no
// adapter, no library load, no log line, no event-log entry).  With one
// path alone the adapter is built and answers kUnavailable with the shared
// half-configuration line, before any library loads, exactly as on the
// Apache port.  With both set the check runs.  `site_handler` is registered
// with the registry so the adapter's announcements reach the site's live
// log while that handler lives.
IisDaemonCheckRegistry::Result CheckSite(const GoogleString& socket_path,
                                         const GoogleString& volume_path,
                                         MessageHandler* site_handler);

}  // namespace net_instaweb

#endif  // PAGESPEED_IIS_IIS_DAEMON_CHECK_H_
