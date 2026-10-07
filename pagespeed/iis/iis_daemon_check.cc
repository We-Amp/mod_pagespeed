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

#include "pagespeed/iis/iis_daemon_check.h"

#include <algorithm>
#include <vector>

#include "pagespeed/kernel/base/message_handler.h"
#include "pagespeed/iis/iis_event_log.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace net_instaweb {

IisDaemonCheckRegistry* IisDaemonCheckRegistry::Get() {
	// The no-destructor idiom: the registry must outlive every context that
	// borrowed an adapter, and a destructor would run during static
	// destruction where a late GetServerContext would touch a destroyed
	// mutex.  The storage is static, so it is not a leak in any tool's
	// sense either.
	static IisDaemonCheckRegistry* registry = new IisDaemonCheckRegistry;
	return registry;
}

// The registry's own message handler and the live site handlers it forwards
// to.  An adapter the registry owns outlives any one site context: a
// configuration edit deletes the old context and with it the handler its
// factory owned, so an adapter must never hold a site's handler directly --
// its messages would reach a deleted object after the first edit.  Instead
// every adapter logs through ForwardingHandler, which holds nothing but
// this pool and delivers each message to the handlers registered as live
// RIGHT NOW.
//
// Lock order: the registry's mu_ (held while an adapter's StartupCheck
// runs) or the adapter's own mutex (its record-cache retry path), then the
// pool's mutex, then each site handler's own lock inside Deliver.  No site
// handler calls back into the pool or the registry, so there is no cycle.
struct IisDaemonCheckRegistry::SiteHandlerPool {
	class ForwardingHandler : public MessageHandler {
	 public:
		explicit ForwardingHandler(SiteHandlerPool* pool) : pool_(pool) {}

	 protected:
		void MessageSImpl(MessageType type,
		                  const GoogleString& message) override {
			pool_->Deliver(type, message);
		}
		void FileMessageSImpl(MessageType type, const char* /*filename*/,
		                      int /*line*/,
		                      const GoogleString& message) override {
			pool_->Deliver(type, message);
		}

	 private:
		SiteHandlerPool* pool_;
	};

	void Deliver(MessageType type, const GoogleString& message) {
		std::lock_guard<std::mutex> lock(mu);
		if (capture != nullptr && type >= kError) {
			if (!capture->empty()) {
				capture->append(" ");
			}
			capture->append(message);
		}
		for (MessageHandler* sink : sinks) {
			sink->MessageS(type, message);
		}
	}

	// While set, error-level messages are also appended to *target, so the
	// caller can repeat the check's own words where an operator reads them.
	void SetCapture(GoogleString* target) {
		std::lock_guard<std::mutex> lock(mu);
		capture = target;
	}

	void Add(MessageHandler* handler) {
		if (handler == nullptr) {
			return;
		}
		std::lock_guard<std::mutex> lock(mu);
		if (std::find(sinks.begin(), sinks.end(), handler) == sinks.end()) {
			sinks.push_back(handler);
		}
	}

	void Remove(MessageHandler* handler) {
		std::lock_guard<std::mutex> lock(mu);
		sinks.erase(std::remove(sinks.begin(), sinks.end(), handler),
		            sinks.end());
	}

	void RemoveAll() {
		std::lock_guard<std::mutex> lock(mu);
		sinks.clear();
	}

	std::mutex mu;
	std::vector<MessageHandler*> sinks;
	GoogleString* capture = nullptr;
	ForwardingHandler handler{this};
};

IisDaemonCheckRegistry::IisDaemonCheckRegistry()
    : pool_(new SiteHandlerPool) {}

IisDaemonCheckRegistry::Result IisDaemonCheckRegistry::GetOrCreate(
    const GoogleString& library_path, const GoogleString& socket_path,
    const GoogleString& volume_path) {
	Result result;
	const auto key = std::make_tuple(library_path, socket_path, volume_path);
	std::lock_guard<std::mutex> lock(mu_);
	Entry& entry = entries_[key];
	if (entry.adapter == nullptr) {
		// First sight of this triple in the process: bind, check, and keep.
		// The library is loaded once per distinct triple and never unloaded
		// (the shared loader's rule), so repeated configuration rebuilds
		// reuse this adapter instead of growing the loader's reference
		// count.  The adapter logs through the registry's own handler,
		// which forwards to whatever site handlers are live at the moment.
		++binder_calls_;
		std::unique_ptr<DaemonAdapter> adapter(
		    new DaemonAdapter(socket_path, volume_path, &pool_->handler));
		adapter->set_library_path(library_path);
		adapter->set_abi_loader(loader_);
		// The check's error text is kept: on a split it names the volume
		// file the check's own open created, and on IIS the event-log
		// entry below is the one place an operator can read that name --
		// the site's own log is not reachable once it has refused.
		GoogleString check_error;
		pool_->SetCapture(&check_error);
		const DaemonStartupStatus status = adapter->StartupCheck();
		pool_->SetCapture(nullptr);
		entry.split = (status == DaemonStartupStatus::kRefuseToStart);
		entry.adapter = std::move(adapter);

		if (entry.split) {
			// ONE event-log entry per distinct split message per process,
			// however many sites and rebuilds see it.  The adapter's own
			// latch already keeps the error-log line to one per distinct
			// condition; this extends that to the event log.  The event
			// log is initialised HERE, under this lock: the facility is
			// idempotent, and this is the module's one writer.
			GoogleString message = StrCat(
			    "The pagespeed module refused to engage for a site: opening "
			    "the optimizer daemon's cache volume at ",
			    volume_path,
			    " created a second volume file instead of attaching to the "
			    "daemon's, so the site engages no PageSpeed at all rather "
			    "than risk two separate caches. Start or restart the "
			    "optimizer daemon and let it create its volume, then recycle "
			    "the application pool; the check's report, when it "
			    "follows, says what became of the volume file its open "
			    "created -- removed, still there, not confirmed gone, or "
			    "left in place because another user or process holds it. "
			    "If it still refuses after that, "
			    "the module and the daemon disagree about the volume: "
			    "install a module and optimizer package pair that agree.");
			// De-duplicated on the fixed text above.  The check's own words
			// follow it only in the entry: they come through the adapter's
			// once-per-process announcement, so a second adapter over the
			// same volume would bring none and must not add a second entry.
			if (split_logged_.insert(message).second) {
				if (!check_error.empty()) {
					StrAppend(&message, " The check reported: ", check_error);
				}
				InitEventLog();
				LogEvent(EventLogLevel::kError, message.c_str());
			}
		}
	}
	result.adapter = entry.adapter.get();
	result.split = entry.split;
	return result;
}

void IisDaemonCheckRegistry::RegisterSiteHandler(MessageHandler* handler) {
	pool_->Add(handler);
}

void IisDaemonCheckRegistry::UnregisterSiteHandler(MessageHandler* handler) {
	pool_->Remove(handler);
}

void IisDaemonCheckRegistry::ResetForTesting() {
	std::lock_guard<std::mutex> lock(mu_);
	entries_.clear();
	split_logged_.clear();
	binder_calls_ = 0;
	// Restore the production binder: a previous test's lambda holds a
	// reference to that test's dead stack object, and a later test that
	// wants the REAL binder must not call through it.
	loader_ = &LoadDaemonAbi;
	pool_->RemoveAll();
}

GoogleString DefaultDaemonLibraryPath() {
#ifdef _WIN32
	// Resolve THIS module's own binary from an address inside this
	// translation unit: the handle is taken without touching the loader's
	// reference count, and an address in the module is valid from the
	// first instruction, unlike any global the module fills in later.
	HMODULE self = nullptr;
	if (!GetModuleHandleExW(
	        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
	            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	        reinterpret_cast<LPCWSTR>(&DefaultDaemonLibraryPath), &self)) {
		return kDefaultDaemonLibraryName;
	}
	// GetModuleFileNameW grows: a call that filled the buffer completely
	// was truncated, so retry with a larger one.
	std::wstring wide;
	DWORD capacity = MAX_PATH;
	for (;;) {
		wide.resize(capacity);
		const DWORD written = GetModuleFileNameW(self, &wide[0], capacity);
		if (written == 0) {
			return kDefaultDaemonLibraryName;
		}
		if (written < capacity) {
			wide.resize(written);
			break;
		}
		capacity *= 2;
	}
	// Keep the directory and drop the file name; the separator stays, so
	// the join below can never produce a bare name.
	const size_t slash = wide.find_last_of(L"\\/");
	if (slash == std::wstring::npos) {
		return kDefaultDaemonLibraryName;
	}
	wide.resize(slash + 1);
	// UTF-16 to UTF-8, explicitly: the registry's key, the loader's own
	// messages and the site's log all spell the path in UTF-8.
	GoogleString directory;
	const int bytes =
	    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(),
	                        static_cast<int>(wide.size()), nullptr, 0,
	                        nullptr, nullptr);
	if (bytes <= 0) {
		return kDefaultDaemonLibraryName;
	}
	directory.resize(bytes);
	WideCharToMultiByte(CP_UTF8, 0, wide.c_str(),
	                    static_cast<int>(wide.size()), &directory[0], bytes,
	                    nullptr, nullptr);
	return StrCat(directory, kDefaultDaemonLibraryName);
#else
	// The POSIX loader searches shared-library directories, so the bare
	// file name is a usable default there; this code is built for the IIS
	// port, which is Windows.
	return GoogleString(kDefaultDaemonLibraryName);
#endif
}

IisDaemonCheckRegistry::Result CheckSite(const GoogleString& socket_path,
                                         const GoogleString& volume_path,
                                         MessageHandler* site_handler) {
	if (socket_path.empty() && volume_path.empty()) {
		// Both options unset: nothing runs at all.  The registry is not
		// even reached -- no adapter object, no library load, no log line,
		// no event-log entry.
		return IisDaemonCheckRegistry::Result();
	}
	IisDaemonCheckRegistry* registry = IisDaemonCheckRegistry::Get();
	registry->RegisterSiteHandler(site_handler);
	return registry->GetOrCreate(DefaultDaemonLibraryPath(), socket_path,
	                             volume_path);
}

}  // namespace net_instaweb
