// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#define _WINSOCKAPI_
#include <Windows.h>
#include "Shlwapi.h"


#include <re2/re2.h>
#include <atomic>
#include <functional>
#include <vector>
#include <algorithm>
#include <string>

#include "pagespeed/iis/iis_config_util.h"
#include "pagespeed/iis/iis_configuration.h"
#include "pagespeed/iis/iis_rewrite_options.h"
#include "pagespeed/kernel/base/stdio_file_system.h"

using namespace std;
using namespace net_instaweb;


class ReferenceCounter
{
protected:
	std::atomic<unsigned int> reference;
public:
	ReferenceCounter() {reference=1;}
	virtual ~ReferenceCounter() {};
	void Addref() {reference.fetch_add(1);}
	void ReleaseRef() {if (reference.fetch_sub(1)==1) delete this;}
};



struct ConfigLine {
	ConfigLine(string key,string value,int mode):valuematch(value) {this->key=key;/*this->value=value*/;this->mode=mode;this->rewriteoptions=NULL;}
	string key;
	//string value;
	RE2 valuematch;
	IisRewriteOptions *rewriteoptions;
	int mode;

	~ConfigLine() {if (rewriteoptions) delete rewriteoptions;}
};
class ConfigurationFile:public ReferenceCounter
{
	list<ConfigLine*> matchLines;
	DWORD64 expiresat;
	FILETIME orgtime;

	// Per-instance critical section that serializes Expired() invocations.
	//
	// Expired() mutates `expiresat` (and reads `path` / `orgtime`) and is
	// called from ConfigFactory::GetConfiguration while the caller holds the
	// factory's SRWLock only as SHARED. A shared lock does not serialize
	// readers against each other, so two concurrent IIS worker threads
	// hitting the same cached ConfigurationFile* race on these non-atomic
	// fields and on the CreateFileA/GetFileTime file I/O. This is a real
	// data race regardless of whether the platform-specific verifier
	// instrumentation catches it today; see the deferred peer-defect
	// list.
	//
	// We use a CRITICAL_SECTION (cheap on Windows, recursive-safe within a
	// single thread, no kernel transition in the uncontended case) rather
	// than promoting the factory's SRWLock to exclusive on every cache hit:
	// promoting would serialize all readers across the entire factory for
	// an I/O-bound check that is per-instance.
	CRITICAL_SECTION expired_lock_;

	string path;

	virtual ~ConfigurationFile()
	{
		for(list<ConfigLine*>::iterator it=matchLines.begin();it!=matchLines.end();it++)
		{
			ConfigLine *c=*it;
			delete c;
		}
		DeleteCriticalSection(&expired_lock_);
	}

	vector<string> tokenize(const string &s, int(*fp) (int)) {
		return iis_config_util::tokenize(s, fp);
	}

	// R2: Extracted from LoadFile. Parses config text into matchLines.
	// R3: expand_env allows injection of environment variable expansion
	// for testing. When nullptr, uses real ExpandEnvironmentStringsA.
	void ParseConfigText(const char* text, size_t length,
	                     MessageHandler* mh,
	                     net_instaweb::global_settings& global_config,
	                     std::function<std::string(const std::string&)> expand_env)
	{
		IisRewriteOptions *currewriteoptions=NULL;
		// item 3: options appearing after a match rule are per-request (matched)
		// scope; process/server-only directives are rejected there.
		bool seen_match_rule=false;

		// Make a mutable copy with null terminator. This copies the buffer
		// even when LoadFile already provides a mutable one — intentional,
		// because the test path (CreateTestConfigFile) passes a string's
		// internal buffer which must not be mutated.
		char* tempbuf = new char[length + 1];
		memcpy(tempbuf, text, length);
		tempbuf[length] = 0;

		char *ptr=tempbuf;
		while(*ptr)
		{
			char *startline=ptr;
			string value;
			int mode=0;



			if (*ptr!='#') // if not a comment
			{

				while(*ptr && *ptr!=':' && *ptr!=' ' && *ptr!='\n' && *ptr!='\r') ptr++;
				char *endid=ptr;
				if (*ptr==0 || *ptr=='\n' || *ptr=='\r') mode=2; // option
				else
				if (*ptr==' ') // option
				{
					mode=2;
					ptr++;

				}
				else
				if(*ptr==':') // match rule
				{
					mode=1;
					ptr++;
				}
				char *value=ptr;
				while(*ptr && *ptr!='\n' && *ptr!='\r') ptr++;
				if (mode==1)
				{
					ConfigLine *matchline=new ConfigLine(std::string(startline,endid-startline),std::string(value,ptr-value),mode);
					if (!matchline->valuematch.ok())
					{
						mh->Message(kWarning, "Invalid config match regex [%s]: %s", matchline->valuematch.pattern().c_str(), matchline->valuematch.error().c_str());
					}
					matchLines.push_back(matchline);
					seen_match_rule=true;
				}
				else
					if (mode==2) //fix
					{
						bool addedconfig=false;
						if (!matchLines.size()) // If no config, then we match base config
						{
							addedconfig=true;
							matchLines.push_back(new ConfigLine(std::string("config"),std::string("base"),3) );
						}

						ConfigLine *currentLine=matchLines.back();
						if (!currentLine->rewriteoptions)
						{
							if (!addedconfig) // if we do not have base config, we add request level indicator
							{
							// check impact
								currentLine=new ConfigLine(std::string("config"),std::string("request"),1);
								matchLines.push_back(currentLine );
							}
							// TODO(oschaaf): FIXME
							currentLine->rewriteoptions=new IisRewriteOptions(NULL);
						}
						currewriteoptions=currentLine->rewriteoptions;


						string option=string(startline,endid-startline);
						string svalue=string(value,ptr-value);
						currentLine->mode|=8;

						if (StringCaseEqual(option,"clear"))
							currentLine->mode|=4;
						else
						if (option=="!")
						{
							mh->Message(kWarning, "The '!' directive is not supported and has no effect; ignoring.");
							currentLine->mode|=2;
						}
						else
						if (StringCaseEqual(option,"filters"))
							currewriteoptions->AdjustFiltersByCommaSeparatedList(svalue,mh);
						else
						if (StringCaseEqual(option,"forbiddenfilters"))
							currewriteoptions->ForbidFiltersByCommaSeparatedList(svalue,mh);
						else
						if (StringCaseEqual(option.substr(0,7), "header_"))
							currewriteoptions->AddCustomFetchHeader(option.substr(7),svalue);
						else if (StringCaseEqual(option,"pagespeed")
							|| StringCaseEqual(option,"iispeed")
							|| StringCaseEqual(option,"ModPagespeed")
							)
						{
							vector<string> tokens = tokenize(svalue, isspace);

							// Bug 2 fix: use a flag instead of 'continue' to avoid
							// skipping end-of-line advancement.
							bool skip_option_parsing = false;
							if (tokens.size() == 2 &&
								(StringCaseEqual(tokens.at(0), "RemoteConfigurationUrl"))) {
								skip_option_parsing = true;
							}

							if (!skip_option_parsing && tokens.size() == 2 &&
								(StringCaseEqual(tokens.at(0), "FileCachePath")
								 || StringCaseEqual(tokens.at(0), "LogDir")
								) )
							{
								// R3: Use injected expand_env if provided, otherwise
								// use real ExpandEnvironmentStringsA.
								if (expand_env) {
									tokens[1] = expand_env(tokens[1]);
								} else {
									// Bug 1 fix: two-pass ExpandEnvironmentStringsA
									// to handle paths longer than MAX_PATH.
									DWORD required = ExpandEnvironmentStringsA(
									    tokens[1].c_str(), nullptr, 0);
									if (required > 0) {
										std::vector<char> buf(required);
										DWORD written = ExpandEnvironmentStringsA(
										    tokens[1].c_str(), buf.data(), required);
										if (written > 0 && written <= required) {
											tokens[1].assign(buf.data());
										}
									}
								}
								// oschaaf: if the configuration did not specify an internal file name
								// we must translate the windows path to an internal filename
								if (tokens[1][0] != '/')
								{
									std::replace(tokens[1].begin(), tokens[1].end(), '\\', '/');
								}
							}
							else if (!skip_option_parsing && tokens.size() == 3 &&
								(StringCaseEqual(tokens.at(0), "LoadFromFile") ||
								StringCaseEqual(tokens.at(0), "LoadFromFileMatch")
								)
								)
							{
								// oschaaf: if the configuration did not specify an internal file name
								// we must translate the windows path to an internal filename
								if (tokens[2][0] != '/')
								{
									std::replace(tokens[2].begin(), tokens[2].end(), '\\', '/');
								}
							}

							// oschaaf: consider moving ownership of all this to the process context
							// ks: let us not do this, because the process context is not the owner
							// of the configuration files, reloading configurations will reset the process context
							// This emits a useless message - which is ignored.
							// the call itself handles debug output emission

							if (!skip_option_parsing) {
								// item 3: reject process/server-scoped directives inside a
								// matched (per-request) block; warn and ignore.
								StringPiece scope_directive;
								if (!tokens.empty()) {
									scope_directive = tokens[0];
									StringPiece mod_pagespeed_prefix("ModPagespeed");
									if (StringCaseStartsWith(scope_directive, mod_pagespeed_prefix)) {
										scope_directive.remove_prefix(mod_pagespeed_prefix.size());
									}
								}
								if (seen_match_rule && !tokens.empty() &&
									currewriteoptions->GetOptionScope(scope_directive) >
										RewriteOptions::kDirectoryScope) {
									mh->Message(kWarning, "Directive [%s] cannot be set inside a match block; ignoring.", svalue.c_str());
								} else {
									currewriteoptions->ParseAndSetOptions(tokens, mh, global_config);
								}
							}
						}
						else
						{
							StringPiece scope_directive(option);
							StringPiece mod_pagespeed_prefix("ModPagespeed");
							if (StringCaseStartsWith(scope_directive, mod_pagespeed_prefix)) {
								scope_directive.remove_prefix(mod_pagespeed_prefix.size());
							}
							if (seen_match_rule &&
								currewriteoptions->GetOptionScope(scope_directive) >
									RewriteOptions::kDirectoryScope) {
								mh->Message(kWarning, "Directive [%s] cannot be set inside a match block; ignoring.", option.c_str());
							} else {
								GoogleString msg;
								RewriteOptions::OptionSettingResult r =
									currewriteoptions->SetOptionFromName(StringPiece(option),GoogleString(svalue),&msg);
								if (r != RewriteOptions::kOptionOk) {
									mh->Message(kWarning, "Failed to set option [%s]: %s", option.c_str(), msg.c_str());
								}
							}
						}
					}
			}
			while (*ptr!='\n' && *ptr && *ptr!='\r') ptr++; // ignore further content
			while (*ptr=='\n' || *ptr=='\r') ptr++; // ignore newline and lf
		}

		delete[] tempbuf;
	}


	void LoadFile(string path,MessageHandler *mh, net_instaweb::global_settings& global_config)
	{
		HANDLE file=CreateFileA(path.c_str(),GENERIC_READ ,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
		//mh->Message(kInfo, "read file [%s]", path.c_str());
		if (file!=INVALID_HANDLE_VALUE)
		{
			// Bug 3 fix: use DWORD instead of int to avoid signed overflow
			// for files > 2GB. INVALID_FILE_SIZE (0xFFFFFFFF) is also returned
			// for legitimate 4GB-1 files, but the 100MB cap rejects those anyway.
			DWORD size=GetFileSize(file,NULL);
			if ((size == INVALID_FILE_SIZE && GetLastError() != NO_ERROR)
			    || size > (100 * 1024 * 1024)) {
				CloseHandle(file);
				expiresat=GetTickCount64()+1000;
				return;
			}
			DWORD read;
			// Bug 4 fix: check GetFileTime return value.
			if (!GetFileTime(file,NULL,NULL,&orgtime)) {
				// If GetFileTime fails, orgtime stays zero-initialized
				// (from constructor). Expired() will reload on next check.
			}
			this->path=path;

			char *tempbuf=new char [size+1];
			bool ret=ReadFile(file,tempbuf,size,&read,NULL);
			CloseHandle(file);

			if (ret)
			{
				tempbuf[read]=0;
				// R2: delegate to ParseConfigText
				ParseConfigText(tempbuf, read, mh, global_config, nullptr);
			}
			else this->path="";


			delete [] tempbuf;
		}
		expiresat=GetTickCount64()+1000;


	}

public:
	ConfigurationFile(string path,MessageHandler *mh, global_settings& global_config):ReferenceCounter() {
		// Bug 4 fix: zero-initialize orgtime so Expired() behaves
		// predictably if GetFileTime fails.
		memset(&orgtime, 0, sizeof(orgtime));
		InitializeCriticalSection(&expired_lock_);
		LoadFile(path,mh, global_config);

	}

	// Test-only constructor: creates an empty ConfigurationFile (no file I/O).
	ConfigurationFile():ReferenceCounter(), expiresat(0) {
		memset(&orgtime, 0, sizeof(orgtime));
		InitializeCriticalSection(&expired_lock_);
	}

	bool Expired()
	{
		// SRWLock-safety: serialize Expired() invocations on this instance.
		//
		// ConfigFactory::GetConfiguration calls this method on cached entries
		// while the caller holds the factory's SRWLock only SHARED. A shared
		// lock does not serialize readers against each other, so without this
		// per-instance critical section two concurrent IIS worker threads can
		// race on expiresat (writes at "expiresat=GetTickCount64()+1000;"
		// below) and on the CreateFileA/GetFileTime file I/O.
		// sibling defects.
		EnterCriticalSection(&expired_lock_);
		bool expired=expiresat<GetTickCount64();
		if (expired && path!="")
		{
			HANDLE file=CreateFileA(path.c_str(),GENERIC_READ ,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
			if (file!=INVALID_HANDLE_VALUE)
			{
				FILETIME ftc;
				if (GetFileTime(file,NULL,NULL,&ftc))
				{
					if (ftc.dwHighDateTime==orgtime.dwHighDateTime
						&&
						ftc.dwLowDateTime==orgtime.dwLowDateTime
						)
					{
						expired=false;
						expiresat=GetTickCount64()+1000;
					}
				}
				CloseHandle(file);
			}

		}
		LeaveCriticalSection(&expired_lock_);

		return expired;
	}
	// returns true if stop matching is found
	bool GetConfig(map<string,string> &input,RewriteOptions &options)
	{
		int mode=0;
		int matches=0;
		bool stopmatching=false;
		bool matched=true;
		// TODO(oschaaf): FIXME
		IisRewriteOptions *ret=new IisRewriteOptions(NULL);
		for(list<ConfigLine*>::iterator it=matchLines.begin();it!=matchLines.end();it++)
		{
			ConfigLine *c=*it;

			if (matched)
			{
				string matchwith=input[c->key];

				matched=re2::RE2::FullMatch(matchwith,c->valuematch);
				if (matched)
				{
					if (c->mode&4) //clear
					{
						delete ret;
						// TODO(oschaaf): FIXME
						ret=new IisRewriteOptions(NULL);

					}
					if (matched && c->mode&8) //last -> merge
					{
						matches++;
						ret->Merge(*c->rewriteoptions);
					}
					if (matched && c->mode&4) // no more matches
					{
						stopmatching=true;
						break;
					}
				}

			}
			if (c->mode!=1) matched=true; // setup for next match
		}
		if (matches) options.Merge(*ret);
		delete ret;
		return stopmatching;
	}

	// Test-only: allow CreateTestConfigFile to call ParseConfigText.
	friend ConfigurationFile* CreateTestConfigFile(
	    const std::string& text,
	    MessageHandler* mh,
	    global_settings& config,
	    std::function<std::string(const std::string&)> expand_env);


};


// Test factory: creates a ConfigurationFile from raw text without file I/O.
ConfigurationFile* CreateTestConfigFile(
    const std::string& text,
    MessageHandler* mh,
    global_settings& config,
    std::function<std::string(const std::string&)> expand_env) {
	ConfigurationFile* cf = new ConfigurationFile();
	cf->ParseConfigText(text.data(), text.size(), mh, config, expand_env);
	return cf;
}

// Test helper: call GetConfig on a ConfigurationFile.
bool TestConfigFileGetConfig(
    ConfigurationFile* cf,
    std::map<std::string, std::string>& input,
    RewriteOptions& options) {
	return cf->GetConfig(input, options);
}

// Test helper: release a ConfigurationFile reference.
void TestConfigFileRelease(ConfigurationFile* cf) {
	cf->ReleaseRef();
}


ConfigFactory::ConfigFactory() {InitializeSRWLock(&rwlock);}
ConfigFactory::~ConfigFactory() {

	map<string,ConfigurationFile *>::iterator it=configurationFiles.begin();
	for (;it!=configurationFiles.end();it++)
	{
		it->second->ReleaseRef();
	}
}
bool ConfigFactory::GetConfig(list<string> paths,map<string,string> &input,RewriteOptions &rwo,MessageHandler *mh,
							  net_instaweb::global_settings* global_config)
{
	//CHECK(global_config != NULL);
	net_instaweb::global_settings fake;
	if (global_config == nullptr) {
		global_config = &fake;
	}
	list<ConfigurationFile *> configs;

	for(auto it=paths.begin();it!=paths.end();it++)
	{
		configs.push_back(GetConfiguration(*it,mh, *global_config));
	}

	bool checkmatch=true;
	for (auto it=configs.begin();it!=configs.end();it++)
	{
		if (checkmatch)
		{
			checkmatch=!(*it)->GetConfig(input,rwo);
		}
		(*it)->ReleaseRef();
	}
	//ConfigurationFile *files[]=new [paths.size()];

	return true;
}
ConfigurationFile *ConfigFactory::GetConfiguration(string path,MessageHandler *mh,
												   net_instaweb::global_settings& global_config)
{
	// SRWLock-safety: do NOT use std::map::operator[] under a shared lock.
	//
	// operator[] inserts a default-constructed value when the key is missing,
	// which mutates the map. A shared (reader) lock does not serialize against
	// other readers, so concurrent readers can race and corrupt the map's
	// internal red-black tree. AppVerifier's SRWLock and Locks providers also
	// flag write-shaped mutation under a shared lock as a verifier stop.
	// (See the historical "// x times a lock we must fix this" comment.)
	//
	// Use find() under the shared lock; if the entry is missing or expired,
	// drop the shared lock, allocate, and reacquire exclusive. Re-check the
	// map under the exclusive lock to avoid double-insert races between two
	// threads that both saw a missing entry.
	ConfigurationFile *file = NULL;
	AcquireSRWLockShared(&rwlock);
	auto it = configurationFiles.find(path);
	if (it != configurationFiles.end()) {
		ConfigurationFile *candidate = it->second;
		if (candidate && !candidate->Expired()) {
			candidate->Addref();
			file = candidate;
		}
	}
	ReleaseSRWLockShared(&rwlock);

	if (!file) {
		ConfigurationFile *newfile =
		    new ConfigurationFile(path, mh, global_config);
		AcquireSRWLockExclusive(&rwlock);
		// Re-check under exclusive lock: another thread may have populated
		// the entry between our shared-lock release and exclusive-lock
		// acquire.
		auto exclusive_it = configurationFiles.find(path);
		if (exclusive_it != configurationFiles.end() &&
		    exclusive_it->second != nullptr &&
		    !exclusive_it->second->Expired()) {
			// Someone beat us to it. Use theirs, discard ours.
			file = exclusive_it->second;
			file->Addref();
			ReleaseSRWLockExclusive(&rwlock);
			newfile->ReleaseRef();
		} else {
			if (exclusive_it != configurationFiles.end() &&
			    exclusive_it->second != nullptr) {
				exclusive_it->second->ReleaseRef();
			}
			configurationFiles[path] = newfile;
			newfile->Addref();
			file = newfile;
			ReleaseSRWLockExclusive(&rwlock);
		}
	}
	return file;
}



