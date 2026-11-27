// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

// iis_pipeline_probe.cc - a stand-alone IIS native module that records what
// a module sees in the IIS request pipeline. It is test equipment, not part
// of the product: probe_iis_html_cache_pipeline.ps1 builds it, installs one
// or more copies of it next to the PageSpeed module in a test guest, drives
// requests and reads the records back.
//
// Each copy is registered under its own module name ("PipeProbeA",
// "PipeProbeB", ...); the last letter of the name is the copy's letter. A copy
// registers the notifications the PageSpeed module registers, at the same
// priorities (RQ_RESOLVE_REQUEST_CACHE at PRIORITY_ALIAS_FIRST,
// RQ_SEND_RESPONSE at PRIORITY_ALIAS_LAST), so a copy placed beside the
// PageSpeed module in <modules> sees what the PageSpeed module sees. A copy
// whose name ends in "F", "G" or "H" registers RQ_SEND_RESPONSE at
// PRIORITY_ALIAS_FIRST instead, to show what the other end of the send
// order sees. IIS calls RegisterModule once per module entry, so two
// entries may name one image (the probe scripts register "G" on A's image,
// and the serve-cached-html matrix "H" on B's) and each registration keeps
// its own name, id and priorities.
//
// Every notification is written as one line to
//   C:\pipeprobe\log\<module name>-<pid>.log
// as "id=<X-Probe-Id>|t=<us>|pid=..|tid=..|mod=..|ev=..|<fields>".
//
// Requests choose actions with the request header
//   X-Probe-Act: <letter>.<action>[,<letter>.<action>...]
// Actions (only the copy whose letter matches acts):
//   serve     at RESOLVE_REQUEST_CACHE: answer with a small text/markdown
//             body and the Markdown header set, the way the PageSpeed
//             module answers from its cache (cache disarm, SetHeader,
//             WriteEntityChunks, FINISH_REQUEST)
//   serve304  as serve, with a 304 carrying ETag, Cache-Control and Vary
//   nodisarm  with serve or serve_html: leave IIS's response caches armed
//   strip     at the first send that carries headers: delete every header
//             outside the Markdown allowlist from the response
//   nc_ae     at RESOLVE_REQUEST_CACHE: delete the request's
//             Accept-Encoding header
//   nc_sv     at RESOLVE_REQUEST_CACHE: set the server variable
//             IIS_EnableDynamicCompression to 0
//   nc_ce     with serve: answer with "Content-Encoding: identity"
//   rw        at the first send with no more data: clear the response and
//             return PENDING; a worker thread then writes a replacement
//             body by reference and resumes the request with
//             IndicateCompletion(CONTINUE), as the PageSpeed module's HTML
//             path does after a rewrite
//   rwflush   as rw, and the worker thread also calls Flush(sync, more)
//             before it resumes the request
//   pid       at the first send that carries headers: add X-Probe-Pid
//   bindings  at RESOLVE_REQUEST_CACHE: read system.applicationHost/sites
//             through the admin manager and record every site and binding
//             this worker process can see
//   capture   at every send: copy the entity this copy sees (memory and
//             file chunks) and, at the first send that carries headers,
//             the header table; after the last send write them to
//             C:\pipeprobe\stored\<name>.body and <name>.hdrs, where
//             <name> is the request header X-Probe-Stored
//   serve_html at RESOLVE_REQUEST_CACHE: answer with status 200, the
//             header lines of C:\pipeprobe\stored\<name>.replay and the
//             bytes of <name>.body (none for HEAD), then FINISH_REQUEST:
//             the way a module answers with a stored page that the rest of
//             the send path, the PageSpeed module's rewriter included, is
//             meant to see as an origin response. Content-Length is the
//             body's size. Disarms IIS's response caches unless nodisarm
//   ctx       at RESOLVE_REQUEST_CACHE: set a module context under this
//             registration's own id. Every send records whether this
//             registration's context and copy B's context are present
//
// Windows only; built with the Visual C++ toolchain by the probe script (no
// Bazel target). ASCII only.

#define _WINSOCKAPI_
#include <windows.h>
#include <ahadmin.h>
#include <httpserv.h>
#include <oleauto.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

IHttpServer* g_server = nullptr;
CRITICAL_SECTION g_log_lock;
INIT_ONCE g_log_once = INIT_ONCE_STATIC_INIT;
std::atomic<long> g_in_send_total{0};

// One per registration: IIS calls RegisterModule once for every module
// entry that names this DLL, so one image can carry several registrations.
struct ModInfo {
  char name[64];
  char letter;
  bool send_first;
  HTTP_MODULE_ID id;
};
thread_local const ModInfo* t_mod = nullptr;
// Each registration's module id, by letter, for the ctx action.
std::atomic<HTTP_MODULE_ID> g_ids[26];

HTTP_MODULE_ID IdOf(char letter) {
  if (letter < 'A' || letter > 'Z') return nullptr;
  return g_ids[letter - 'A'].load();
}

class ProbeContext : public IHttpStoredContext {
 public:
  void CleanupStoredContext() override { delete this; }
};

bool HasContext(IHttpContext* ctx, HTTP_MODULE_ID id) {
  if (id == nullptr) return false;
  IHttpModuleContextContainer* c = ctx->GetModuleContextContainer();
  return c != nullptr && c->GetModuleContext(id) != nullptr;
}

// A stored-page name from the request: [a-z0-9_-] only, else empty.
std::string StoredName(IHttpContext* ctx);

bool ReadWholeFile(const std::string& path, std::string* out) {
  FILE* f = nullptr;
  if (fopen_s(&f, path.c_str(), "rb") != 0 || f == nullptr) return false;
  char buf[4096];
  size_t n;
  out->clear();
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out->append(buf, n);
  fclose(f);
  return true;
}

bool WriteWholeFile(const std::string& path, const std::string& data) {
  FILE* f = nullptr;
  if (fopen_s(&f, path.c_str(), "wb") != 0 || f == nullptr) return false;
  fwrite(data.data(), 1, data.size(), f);
  fclose(f);
  return true;
}

BOOL CALLBACK InitLogLock(PINIT_ONCE, PVOID, PVOID*) {
  InitializeCriticalSection(&g_log_lock);
  return TRUE;
}

const char* const kKnownResponseHeaders[HttpHeaderResponseMaximum] = {
    "Cache-Control",    "Connection",       "Date",
    "Keep-Alive",       "Pragma",           "Trailer",
    "Transfer-Encoding", "Upgrade",         "Via",
    "Warning",          "Allow",            "Content-Length",
    "Content-Type",     "Content-Encoding", "Content-Language",
    "Content-Location", "Content-MD5",      "Content-Range",
    "Expires",          "Last-Modified",    "Accept-Ranges",
    "Age",              "ETag",             "Location",
    "Proxy-Authenticate", "Retry-After",    "Server",
    "Set-Cookie",       "Vary",             "WWW-Authenticate"};

// The Markdown header allowlist plus the transport lines the server core
// writes itself.
bool KeptOnMarkdown(const char* name) {
  static const char* const kKeep[] = {
      "Content-Type", "Content-Length", "Cache-Control", "Vary",
      "X-Robots-Tag", "Age",            "ETag",          "Date",
      "Server",       "Connection",     "Keep-Alive",    "Transfer-Encoding"};
  for (const char* k : kKeep) {
    if (_stricmp(k, name) == 0) return true;
  }
  return false;
}

unsigned long long NowUs() {
  FILETIME ft;
  GetSystemTimePreciseAsFileTime(&ft);
  ULARGE_INTEGER u;
  u.LowPart = ft.dwLowDateTime;
  u.HighPart = ft.dwHighDateTime;
  return u.QuadPart / 10;
}

void Sanitize(std::string* s) {
  for (char& c : *s) {
    if (c == '|' || c == '\r' || c == '\n' || (unsigned char)c < 0x20 ||
        (unsigned char)c > 0x7e) {
      c = '_';
    }
  }
}

void LogLine(const std::string& id, const char* ev, const std::string& rest) {
  char head[256];
  _snprintf_s(head, sizeof(head), _TRUNCATE,
              "id=%s|t=%llu|pid=%lu|tid=%lu|mod=%s|ev=%s|", id.c_str(),
              NowUs(), GetCurrentProcessId(), GetCurrentThreadId(),
              t_mod != nullptr ? t_mod->name : "?", ev);
  std::string line = head;
  std::string body = rest;
  Sanitize(&body);
  line += body;
  line += "\r\n";
  char path[MAX_PATH];
  _snprintf_s(path, sizeof(path), _TRUNCATE, "C:\\pipeprobe\\log\\%s-%lu.log",
              t_mod != nullptr ? t_mod->name : "unknown", GetCurrentProcessId());
  EnterCriticalSection(&g_log_lock);
  FILE* f = nullptr;
  if (fopen_s(&f, path, "ab") == 0 && f != nullptr) {
    fwrite(line.data(), 1, line.size(), f);
    fclose(f);
  }
  LeaveCriticalSection(&g_log_lock);
}

std::string RequestHeader(IHttpContext* ctx, PCSTR name) {
  USHORT len = 0;
  PCSTR v = ctx->GetRequest()->GetHeader(name, &len);
  return (v != nullptr && len > 0) ? std::string(v, len) : std::string();
}

bool HasAction(IHttpContext* ctx, const char* action) {
  std::string acts = RequestHeader(ctx, "X-Probe-Act");
  std::string want = std::string(1, t_mod->letter) + "." + action;
  size_t pos = 0;
  while (pos <= acts.size()) {
    size_t end = acts.find(',', pos);
    if (end == std::string::npos) end = acts.size();
    std::string tok = acts.substr(pos, end - pos);
    while (!tok.empty() && tok.front() == ' ') tok.erase(0, 1);
    while (!tok.empty() && tok.back() == ' ') tok.pop_back();
    if (tok == want) return true;
    pos = end + 1;
  }
  return false;
}

std::string StoredName(IHttpContext* ctx) {
  std::string n = RequestHeader(ctx, "X-Probe-Stored");
  if (n.empty() || n.size() > 40) return std::string();
  for (char c : n) {
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' ||
          c == '-')) {
      return std::string();
    }
  }
  return n;
}

std::string ServerVariable(IHttpContext* ctx, PCSTR name) {
  PCWSTR v = nullptr;
  DWORD len = 0;
  std::string out;
  if (SUCCEEDED(ctx->GetServerVariable(name, &v, &len)) && v != nullptr) {
    for (DWORD i = 0; i < len && v[i]; ++i) {
      out.push_back(v[i] < 0x80 ? static_cast<char>(v[i]) : '?');
    }
  } else {
    out = "(none)";
  }
  return out;
}

// The whole byte range of a file chunk, read without moving any file
// pointer the server relies on.
bool ReadFileChunk(const HTTP_DATA_CHUNK& c, std::string* out) {
  HANDLE h = c.FromFileHandle.FileHandle;
  if (h == nullptr || h == INVALID_HANDLE_VALUE) return false;
  unsigned long long offset = c.FromFileHandle.ByteRange.StartingOffset.QuadPart;
  unsigned long long length = c.FromFileHandle.ByteRange.Length.QuadPart;
  if (length == HTTP_BYTE_RANGE_TO_EOF) {
    LARGE_INTEGER size;
    if (!GetFileSizeEx(h, &size)) return false;
    length = static_cast<unsigned long long>(size.QuadPart) - offset;
  }
  if (length > (16u << 20)) return false;
  std::string buf(static_cast<size_t>(length), '\0');
  size_t done = 0;
  while (done < buf.size()) {
    OVERLAPPED ov = {};
    unsigned long long at = offset + done;
    ov.Offset = static_cast<DWORD>(at & 0xffffffffu);
    ov.OffsetHigh = static_cast<DWORD>(at >> 32);
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    DWORD got = 0;
    BOOL ok = ReadFile(h, &buf[done], static_cast<DWORD>(buf.size() - done),
                       &got, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
      ok = GetOverlappedResult(h, &ov, &got, TRUE);
    }
    CloseHandle(ov.hEvent);
    if (!ok || got == 0) return false;
    done += got;
  }
  out->append(buf);
  return true;
}

std::string HeaderDump(IHttpResponse* resp) {
  HTTP_RESPONSE* raw = resp->GetRawHttpResponse();
  std::string out;
  if (raw == nullptr) return "nullraw";
  for (int i = 0; i < HttpHeaderResponseMaximum; ++i) {
    const HTTP_KNOWN_HEADER& h = raw->Headers.KnownHeaders[i];
    if (h.RawValueLength > 0 && h.pRawValue != nullptr) {
      out += kKnownResponseHeaders[i];
      out += ": ";
      out.append(h.pRawValue, h.RawValueLength);
      out += " ;; ";
    }
  }
  for (USHORT i = 0; i < raw->Headers.UnknownHeaderCount; ++i) {
    const HTTP_UNKNOWN_HEADER& h = raw->Headers.pUnknownHeaders[i];
    out.append(h.pName, h.NameLength);
    out += ": ";
    if (h.pRawValue != nullptr) out.append(h.pRawValue, h.RawValueLength);
    out += " ;; ";
  }
  return out;
}

// The header table as "Name: value" lines, CRLF-terminated.
std::string HeaderLines(IHttpResponse* resp) {
  HTTP_RESPONSE* raw = resp->GetRawHttpResponse();
  std::string out;
  if (raw == nullptr) return out;
  for (int i = 0; i < HttpHeaderResponseMaximum; ++i) {
    const HTTP_KNOWN_HEADER& h = raw->Headers.KnownHeaders[i];
    if (h.RawValueLength > 0 && h.pRawValue != nullptr) {
      out += kKnownResponseHeaders[i];
      out += ": ";
      out.append(h.pRawValue, h.RawValueLength);
      out += "\r\n";
    }
  }
  for (USHORT i = 0; i < raw->Headers.UnknownHeaderCount; ++i) {
    const HTTP_UNKNOWN_HEADER& h = raw->Headers.pUnknownHeaders[i];
    out.append(h.pName, h.NameLength);
    out += ": ";
    if (h.pRawValue != nullptr) out.append(h.pRawValue, h.RawValueLength);
    out += "\r\n";
  }
  return out;
}

// Reads the first two bytes of a file chunk without moving any file
// pointer the server relies on.
std::string FileChunkMagic(const HTTP_DATA_CHUNK& c) {
  HANDLE h = c.FromFileHandle.FileHandle;
  if (h == nullptr || h == INVALID_HANDLE_VALUE) return "nohandle";
  unsigned char buf[2] = {0, 0};
  OVERLAPPED ov = {};
  ov.Offset = c.FromFileHandle.ByteRange.StartingOffset.LowPart;
  ov.OffsetHigh = c.FromFileHandle.ByteRange.StartingOffset.HighPart;
  ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  DWORD got = 0;
  BOOL ok = ReadFile(h, buf, 2, &got, &ov);
  if (!ok && GetLastError() == ERROR_IO_PENDING) {
    ok = GetOverlappedResult(h, &ov, &got, TRUE);
  }
  CloseHandle(ov.hEvent);
  if (!ok || got < 2) return "unread";
  char hex[8];
  _snprintf_s(hex, sizeof(hex), _TRUNCATE, "%02x%02x", buf[0], buf[1]);
  return hex;
}

std::string ChunkDump(IHttpResponse* resp) {
  HTTP_RESPONSE* raw = resp->GetRawHttpResponse();
  if (raw == nullptr) return "chunks=?";
  std::string out;
  char tmp[160];
  unsigned long long mem = 0;
  std::string first_magic;
  std::string text_head;
  for (USHORT i = 0; i < raw->EntityChunkCount; ++i) {
    const HTTP_DATA_CHUNK& c = raw->pEntityChunks[i];
    switch (c.DataChunkType) {
      case HttpDataChunkFromMemory:
        mem += c.FromMemory.BufferLength;
        if (first_magic.empty() && c.FromMemory.BufferLength >= 2) {
          const unsigned char* p =
              static_cast<const unsigned char*>(c.FromMemory.pBuffer);
          _snprintf_s(tmp, sizeof(tmp), _TRUNCATE, "%02x%02x", p[0], p[1]);
          first_magic = tmp;
          size_t n = c.FromMemory.BufferLength < 24 ? c.FromMemory.BufferLength
                                                    : 24;
          text_head.assign(static_cast<const char*>(c.FromMemory.pBuffer), n);
        }
        out += "M";
        break;
      case HttpDataChunkFromFileHandle:
        _snprintf_s(tmp, sizeof(tmp), _TRUNCATE, "F(%lld)",
                    c.FromFileHandle.ByteRange.Length.QuadPart);
        out += tmp;
        if (first_magic.empty()) first_magic = FileChunkMagic(c);
        break;
      case HttpDataChunkFromFragmentCache:
      case HttpDataChunkFromFragmentCacheEx:
        out += "C";
        break;
      default:
        out += "?";
        break;
    }
  }
  _snprintf_s(tmp, sizeof(tmp), _TRUNCATE, "chunks=%u[%s]|mem=%llu|magic=%s|",
              raw->EntityChunkCount, out.c_str(), mem, first_magic.c_str());
  std::string head = text_head;
  Sanitize(&head);
  return std::string(tmp) + "head=" + head;
}

void DisarmCaches(IHttpResponse* response) {
  response->DisableKernelCache();
  IHttpCachePolicy* policy = response->GetCachePolicy();
  if (policy == nullptr) return;
  HTTP_CACHE_POLICY* kernel = policy->GetKernelCachePolicy();
  if (kernel != nullptr) {
    kernel->Policy = HttpCachePolicyNocache;
    kernel->SecondsToLive = 0;
  }
  HTTP_CACHE_POLICY* user = policy->GetUserCachePolicy();
  if (user != nullptr) {
    user->Policy = HttpCachePolicyNocache;
    user->SecondsToLive = 0;
  }
  policy->DisableUserCache();
}

std::string BstrToAscii(BSTR b) {
  std::string s;
  if (b == nullptr) return s;
  for (const wchar_t* p = b; *p; ++p) {
    s.push_back(*p < 0x80 ? static_cast<char>(*p) : '?');
  }
  return s;
}

std::string PropString(IAppHostElement* e, const wchar_t* name) {
  std::string out;
  BSTR bname = SysAllocString(name);
  IAppHostProperty* prop = nullptr;
  if (SUCCEEDED(e->GetPropertyByName(bname, &prop)) && prop != nullptr) {
    VARIANT v;
    VariantInit(&v);
    if (SUCCEEDED(prop->get_Value(&v))) {
      VARIANT s;
      VariantInit(&s);
      if (SUCCEEDED(VariantChangeType(&s, &v, 0, VT_BSTR))) {
        out = BstrToAscii(s.bstrVal);
      }
      VariantClear(&s);
    }
    VariantClear(&v);
    prop->Release();
  }
  SysFreeString(bname);
  return out;
}

void LogBindings(IHttpContext* ctx, const std::string& id) {
  IHttpSite* site = ctx->GetSite();
  char tmp[256];
  _snprintf_s(tmp, sizeof(tmp), _TRUNCATE, "request_site_id=%lu|pool=%ls",
              site != nullptr ? site->GetSiteId() : 0,
              g_server != nullptr ? g_server->GetAppPoolName() : L"?");
  LogLine(id, "bindings.request", tmp);
  IAppHostAdminManager* am =
      g_server != nullptr ? g_server->GetAdminManager() : nullptr;
  if (am == nullptr) {
    LogLine(id, "bindings.error", "no admin manager");
    return;
  }
  BSTR section = SysAllocString(L"system.applicationHost/sites");
  BSTR path = SysAllocString(L"MACHINE/WEBROOT/APPHOST");
  IAppHostElement* sites = nullptr;
  HRESULT hr = am->GetAdminSection(section, path, &sites);
  SysFreeString(section);
  SysFreeString(path);
  if (FAILED(hr) || sites == nullptr) {
    _snprintf_s(tmp, sizeof(tmp), _TRUNCATE, "GetAdminSection hr=0x%08lx",
                static_cast<unsigned long>(hr));
    LogLine(id, "bindings.error", tmp);
    return;
  }
  IAppHostElementCollection* coll = nullptr;
  DWORD count = 0;
  if (SUCCEEDED(sites->get_Collection(&coll)) && coll != nullptr) {
    coll->get_Count(&count);
    for (DWORD i = 0; i < count; ++i) {
      VARIANT idx;
      VariantInit(&idx);
      idx.vt = VT_UI4;
      idx.ulVal = i;
      IAppHostElement* s = nullptr;
      if (FAILED(coll->get_Item(idx, &s)) || s == nullptr) continue;
      std::string line = "name=" + PropString(s, L"name") +
                         "|site_id=" + PropString(s, L"id") + "|bindings=";
      BSTR bn = SysAllocString(L"bindings");
      IAppHostElement* b = nullptr;
      if (SUCCEEDED(s->GetElementByName(bn, &b)) && b != nullptr) {
        IAppHostElementCollection* bc = nullptr;
        DWORD bcount = 0;
        if (SUCCEEDED(b->get_Collection(&bc)) && bc != nullptr) {
          bc->get_Count(&bcount);
          for (DWORD j = 0; j < bcount; ++j) {
            VARIANT bidx;
            VariantInit(&bidx);
            bidx.vt = VT_UI4;
            bidx.ulVal = j;
            IAppHostElement* one = nullptr;
            if (SUCCEEDED(bc->get_Item(bidx, &one)) && one != nullptr) {
              line += "[" + PropString(one, L"protocol") + " " +
                      PropString(one, L"bindingInformation") + "]";
              one->Release();
            }
          }
          bc->Release();
        }
        b->Release();
      }
      SysFreeString(bn);
      LogLine(id, "bindings.site", line);
      s->Release();
    }
    coll->Release();
  }
  _snprintf_s(tmp, sizeof(tmp), _TRUNCATE, "count=%lu", count);
  LogLine(id, "bindings.done", tmp);
  sites->Release();
}

class Probe;

struct RewriteJob {
  IHttpContext* ctx;
  Probe* probe;
  bool flush;
};

class Probe : public CHttpModule {
 public:
  explicit Probe(const ModInfo* info) : info_(info) {}

  REQUEST_NOTIFICATION_STATUS OnBeginRequest(
      IHttpContext* ctx, IHttpEventProvider*) override {
    t_mod = info_;
    id_ = RequestHeader(ctx, "X-Probe-Id");
    if (id_.empty()) id_ = "-";
    const HTTP_REQUEST* raw = ctx->GetRequest()->GetRawHttpRequest();
    char tmp[1024];
    _snprintf_s(tmp, sizeof(tmp), _TRUNCATE,
                "raw=%.*s|cooked_path=%.*ls|method=%s", raw->RawUrlLength,
                raw->pRawUrl, raw->CookedUrl.AbsPathLength / 2,
                raw->CookedUrl.pAbsPath, ctx->GetRequest()->GetHttpMethod());
    LogLine(id_, "begin", tmp);
    return RQ_NOTIFICATION_CONTINUE;
  }

  REQUEST_NOTIFICATION_STATUS OnResolveRequestCache(
      IHttpContext* ctx, IHttpEventProvider*) override {
    t_mod = info_;
    std::string rest = "ae=" + RequestHeader(ctx, "Accept-Encoding");
    PCWSTR pristine = nullptr;
    DWORD plen = 0;
    if (SUCCEEDED(ctx->GetServerVariable("X-PRISTINE-PATH", &pristine,
                                         &plen)) &&
        pristine != nullptr) {
      std::string p;
      for (DWORD i = 0; i < plen && pristine[i]; ++i) {
        p.push_back(pristine[i] < 0x80 ? static_cast<char>(pristine[i]) : '?');
      }
      rest += "|pristine_path=" + p;
    }
    rest += "|pristine_target=" + ServerVariable(ctx, "X-PRISTINE-TARGET") +
            "|script_name=" + ServerVariable(ctx, "SCRIPT_NAME") +
            "|path_info=" + ServerVariable(ctx, "PATH_INFO") +
            "|method=" + ctx->GetRequest()->GetHttpMethod();
    LogLine(id_, "resolve_cache", rest);
    if (HasAction(ctx, "ctx")) {
      IHttpModuleContextContainer* c = ctx->GetModuleContextContainer();
      HRESULT hr = (c != nullptr && info_->id != nullptr)
                       ? c->SetModuleContext(new ProbeContext(), info_->id)
                       : E_FAIL;
      char t[64];
      _snprintf_s(t, sizeof(t), _TRUNCATE, "hr=0x%08lx",
                  static_cast<unsigned long>(hr));
      LogLine(id_, "act.ctx", t);
    }
    if (HasAction(ctx, "serve_html")) return ServeHtml(ctx);
    if (HasAction(ctx, "bindings")) LogBindings(ctx, id_);
    if (HasAction(ctx, "nc_ae")) {
      HRESULT hr = ctx->GetRequest()->DeleteHeader("Accept-Encoding");
      LogLine(id_, "act.nc_ae", hr == S_OK ? "ok" : "failed");
    }
    if (HasAction(ctx, "nc_sv")) {
      HRESULT hr = ctx->SetServerVariable("IIS_EnableDynamicCompression", L"0");
      char t[64];
      _snprintf_s(t, sizeof(t), _TRUNCATE, "hr=0x%08lx",
                  static_cast<unsigned long>(hr));
      LogLine(id_, "act.nc_sv", t);
    }
    if (HasAction(ctx, "serve") || HasAction(ctx, "serve304")) {
      return Serve(ctx);
    }
    return RQ_NOTIFICATION_CONTINUE;
  }

  REQUEST_NOTIFICATION_STATUS OnSendResponse(
      IHttpContext* ctx, ISendResponseProvider* provider) override {
    t_mod = info_;
    const long depth = in_send_++;
    const long global_depth = g_in_send_total++;
    ++sends_;
    IHttpResponse* resp = ctx->GetResponse();
    const DWORD flags = provider->GetFlags();
    const bool more = (flags & HTTP_SEND_RESPONSE_FLAG_MORE_DATA) != 0;
    const bool hbs = provider->GetHeadersBeingSent() != FALSE;
    HTTP_RESPONSE* raw = resp->GetRawHttpResponse();
    char tmp[256];
    _snprintf_s(tmp, sizeof(tmp), _TRUNCATE,
                "seq=%ld|depth=%ld|gdepth=%ld|flags=0x%lx|more=%d|hbs=%d|"
                "status=%u|served=%d|method=%s|own_ctx=%d|b_ctx=%d|",
                sends_, depth, global_depth, flags, more ? 1 : 0, hbs ? 1 : 0,
                raw != nullptr ? raw->StatusCode : 0, served_ ? 1 : 0,
                ctx->GetRequest()->GetHttpMethod(),
                HasContext(ctx, info_->id) ? 1 : 0,
                HasContext(ctx, IdOf('B')) ? 1 : 0);
    LogLine(id_, "send", std::string(tmp) + ChunkDump(resp) +
                             "|hdrs=" + HeaderDump(resp));
    REQUEST_NOTIFICATION_STATUS status = RQ_NOTIFICATION_CONTINUE;
    if (HasAction(ctx, "capture")) Capture(ctx, resp, hbs, more);
    if (hbs && HasAction(ctx, "pid")) {
      char pid[32];
      _snprintf_s(pid, sizeof(pid), _TRUNCATE, "%lu", GetCurrentProcessId());
      resp->SetHeader("X-Probe-Pid", pid, static_cast<USHORT>(strlen(pid)), TRUE);
    }
    if (hbs && HasAction(ctx, "strip")) {
      Strip(resp);
      LogLine(id_, "send.after_strip", "hdrs=" + HeaderDump(resp));
    }
    const bool rw = HasAction(ctx, "rw") || HasAction(ctx, "rwflush");
    if (rw && !more && !rewrote_) {
      rewrote_ = true;
      resp->Clear();
      RewriteJob* job = new RewriteJob{ctx, this, HasAction(ctx, "rwflush")};
      HANDLE t = CreateThread(nullptr, 0, &Probe::RewriteThread, job, 0,
                              nullptr);
      if (t != nullptr) {
        CloseHandle(t);
        LogLine(id_, "act.rw", "pending");
        status = RQ_NOTIFICATION_PENDING;
      } else {
        delete job;
      }
    }
    --g_in_send_total;
    --in_send_;
    _snprintf_s(tmp, sizeof(tmp), _TRUNCATE, "seq=%ld|ret=%d", sends_,
                static_cast<int>(status));
    LogLine(id_, "send.exit", tmp);
    return status;
  }

  REQUEST_NOTIFICATION_STATUS OnAsyncCompletion(
      IHttpContext*, DWORD notification, BOOL post, IHttpEventProvider*,
      IHttpCompletionInfo* info) override {
    t_mod = info_;
    char tmp[128];
    _snprintf_s(tmp, sizeof(tmp), _TRUNCATE,
                "notification=0x%lx|post=%d|bytes=%lu", notification,
                post ? 1 : 0,
                info != nullptr ? info->GetCompletionBytes() : 0);
    LogLine(id_, "async_completion", tmp);
    return RQ_NOTIFICATION_CONTINUE;
  }

  void Dispose() override {
    t_mod = info_;
    LogLine(id_, "dispose", "");
    delete this;
  }

 private:
  static DWORD WINAPI RewriteThread(LPVOID arg) {
    RewriteJob* job = static_cast<RewriteJob*>(arg);
    t_mod = job->probe->info_;
    Sleep(50);
    static char body[] = "REWRITTEN-BYTES-FROM-PROBE\n";
    static HTTP_DATA_CHUNK chunk = {};
    chunk.DataChunkType = HttpDataChunkFromMemory;
    chunk.FromMemory.pBuffer = body;
    chunk.FromMemory.BufferLength = static_cast<ULONG>(strlen(body));
    IHttpResponse* r = job->ctx->GetResponse();
    HRESULT hr = r->WriteEntityChunkByReference(&chunk);
    char tmp[96];
    _snprintf_s(tmp, sizeof(tmp), _TRUNCATE, "write_by_reference hr=0x%08lx",
                static_cast<unsigned long>(hr));
    LogLine(job->probe->id_, "rw.thread", tmp);
    if (job->flush) {
      DWORD sent = 0;
      LogLine(job->probe->id_, "rw.thread", "flush begin");
      hr = r->Flush(FALSE, TRUE, &sent);
      t_mod = job->probe->info_;  // the flush ran later modules here
      _snprintf_s(tmp, sizeof(tmp), _TRUNCATE, "flush end hr=0x%08lx",
                  static_cast<unsigned long>(hr));
      LogLine(job->probe->id_, "rw.thread", tmp);
    }
    LogLine(job->probe->id_, "rw.thread", "indicate completion");
    IHttpContext* ctx = job->ctx;
    std::string id = job->probe->id_;
    delete job;
    const ModInfo* mod = t_mod;
    ctx->IndicateCompletion(RQ_NOTIFICATION_CONTINUE);
    t_mod = mod;  // IndicateCompletion ran later modules on this thread
    LogLine(id, "rw.thread", "after indicate completion");
    return 0;
  }

  void Strip(IHttpResponse* resp) {
    HTTP_RESPONSE* raw = resp->GetRawHttpResponse();
    if (raw == nullptr) return;
    for (int i = 0; i < HttpHeaderResponseMaximum; ++i) {
      if (raw->Headers.KnownHeaders[i].RawValueLength > 0 &&
          !KeptOnMarkdown(kKnownResponseHeaders[i])) {
        resp->DeleteHeader(kKnownResponseHeaders[i]);
      }
    }
    // Copy the names first: DeleteHeader edits the array.
    std::string names[64];
    USHORT n = raw->Headers.UnknownHeaderCount;
    if (n > 64) n = 64;
    for (USHORT i = 0; i < n; ++i) {
      names[i].assign(raw->Headers.pUnknownHeaders[i].pName,
                      raw->Headers.pUnknownHeaders[i].NameLength);
    }
    for (USHORT i = 0; i < n; ++i) {
      if (!KeptOnMarkdown(names[i].c_str())) {
        resp->DeleteHeader(names[i].c_str());
      }
    }
    resp->SetHeader("Cache-Control", "private", 7, TRUE);
  }

  REQUEST_NOTIFICATION_STATUS Serve(IHttpContext* ctx) {
    served_ = true;
    IHttpResponse* r = ctx->GetResponse();
    if (!HasAction(ctx, "nodisarm")) DisarmCaches(r);
    if (HasAction(ctx, "serve304")) {
      r->SetStatus(304, "Not Modified");
      r->SetHeader("ETag", "W/\"psm-probe\"", 13, TRUE);
      r->SetHeader("Cache-Control", "private", 7, TRUE);
      r->SetHeader("Vary", "Accept", 6, TRUE);
      LogLine(id_, "act.serve304", "hdrs=" + HeaderDump(r));
      return RQ_NOTIFICATION_FINISH_REQUEST;
    }
    r->SetStatus(200, "OK");
    static char body[] = "# Probe\n\nA markdown body from the probe.\n";
    const char* kHeaders[][2] = {
        {"Content-Type", "text/markdown; charset=utf-8"},
        {"Cache-Control", "private"},
        {"Vary", "Accept"},
        {"X-Robots-Tag", "noindex"},
        {"ETag", "W/\"psm-probe\""},
        {"Age", "0"}};
    for (auto& h : kHeaders) {
      r->SetHeader(h[0], h[1], static_cast<USHORT>(strlen(h[1])), TRUE);
    }
    if (HasAction(ctx, "nc_ce")) {
      r->SetHeader("Content-Encoding", "identity", 8, TRUE);
    }
    char len[16];
    _snprintf_s(len, sizeof(len), _TRUNCATE, "%u",
                static_cast<unsigned>(strlen(body)));
    r->SetHeader("Content-Length", len, static_cast<USHORT>(strlen(len)),
                 TRUE);
    const bool head = _stricmp(ctx->GetRequest()->GetHttpMethod(), "HEAD") == 0;
    if (!head) {
      HTTP_DATA_CHUNK c = {};
      c.DataChunkType = HttpDataChunkFromMemory;
      c.FromMemory.pBuffer = body;
      c.FromMemory.BufferLength = static_cast<ULONG>(strlen(body));
      DWORD sent = 0;
      r->WriteEntityChunks(&c, 1, FALSE, FALSE, &sent);
    }
    LogLine(id_, "act.serve", "hdrs=" + HeaderDump(r));
    return RQ_NOTIFICATION_FINISH_REQUEST;
  }

  // The entity of every send, and the header table of the first that
  // carries headers; written out after the last send.
  void Capture(IHttpContext* ctx, IHttpResponse* resp, bool hbs, bool more) {
    HTTP_RESPONSE* raw = resp->GetRawHttpResponse();
    if (raw == nullptr) return;
    if (hbs && !capture_has_headers_) {
      capture_has_headers_ = true;
      char st[32];
      _snprintf_s(st, sizeof(st), _TRUNCATE, "%u", raw->StatusCode);
      capture_hdrs_ = std::string("Status: ") + st + "\r\n" + HeaderLines(resp);
    }
    for (USHORT i = 0; i < raw->EntityChunkCount; ++i) {
      const HTTP_DATA_CHUNK& c = raw->pEntityChunks[i];
      if (c.DataChunkType == HttpDataChunkFromMemory) {
        capture_body_.append(static_cast<const char*>(c.FromMemory.pBuffer),
                             c.FromMemory.BufferLength);
      } else if (c.DataChunkType == HttpDataChunkFromFileHandle) {
        if (!ReadFileChunk(c, &capture_body_)) capture_failed_ = true;
      } else {
        capture_failed_ = true;
      }
    }
    if (more) return;
    const std::string name = StoredName(ctx);
    if (name.empty() || capture_failed_) {
      LogLine(id_, "act.capture", capture_failed_ ? "unreadable chunk" : "no name");
      return;
    }
    const std::string base = "C:\\pipeprobe\\stored\\" + name;
    bool ok = WriteWholeFile(base + ".body", capture_body_) &&
              WriteWholeFile(base + ".hdrs", capture_hdrs_);
    char t[96];
    _snprintf_s(t, sizeof(t), _TRUNCATE, "%s|bytes=%zu",
                ok ? "written" : "write failed", capture_body_.size());
    LogLine(id_, "act.capture", t);
  }

  // A stored page answered at RESOLVE_REQUEST_CACHE as an origin response.
  REQUEST_NOTIFICATION_STATUS ServeHtml(IHttpContext* ctx) {
    const std::string name = StoredName(ctx);
    const std::string base = "C:\\pipeprobe\\stored\\" + name;
    std::string replay;
    std::string body;
    if (name.empty() || !ReadWholeFile(base + ".replay", &replay) ||
        !ReadWholeFile(base + ".body", &body)) {
      LogLine(id_, "act.serve_html", "no stored page");
      return RQ_NOTIFICATION_CONTINUE;
    }
    served_ = true;
    IHttpResponse* r = ctx->GetResponse();
    if (!HasAction(ctx, "nodisarm")) DisarmCaches(r);
    r->SetStatus(200, "OK");
    size_t pos = 0;
    while (pos < replay.size()) {
      size_t end = replay.find("\r\n", pos);
      if (end == std::string::npos) end = replay.size();
      std::string line = replay.substr(pos, end - pos);
      pos = end + 2;
      size_t colon = line.find(':');
      if (colon == std::string::npos || colon == 0) continue;
      std::string n = line.substr(0, colon);
      size_t v = colon + 1;
      while (v < line.size() && line[v] == ' ') ++v;
      std::string value = line.substr(v);
      // FALSE: a repeated name adds a line, as the stored table had it.
      r->SetHeader(n.c_str(), value.c_str(), static_cast<USHORT>(value.size()),
                   FALSE);
    }
    char len[24];
    _snprintf_s(len, sizeof(len), _TRUNCATE, "%zu", body.size());
    r->SetHeader("Content-Length", len, static_cast<USHORT>(strlen(len)), TRUE);
    const bool head = _stricmp(ctx->GetRequest()->GetHttpMethod(), "HEAD") == 0;
    if (!head && !body.empty()) {
      // Request memory: IIS and the rewriter read the chunk after this
      // notification has returned.
      void* mem = ctx->AllocateRequestMemory(static_cast<DWORD>(body.size()));
      if (mem == nullptr) {
        LogLine(id_, "act.serve_html", "no request memory");
        return RQ_NOTIFICATION_CONTINUE;
      }
      memcpy(mem, body.data(), body.size());
      HTTP_DATA_CHUNK c = {};
      c.DataChunkType = HttpDataChunkFromMemory;
      c.FromMemory.pBuffer = mem;
      c.FromMemory.BufferLength = static_cast<ULONG>(body.size());
      DWORD sent = 0;
      HRESULT hr = r->WriteEntityChunks(&c, 1, FALSE, FALSE, &sent);
      if (FAILED(hr)) LogLine(id_, "act.serve_html", "write failed");
    }
    char t[64];
    _snprintf_s(t, sizeof(t), _TRUNCATE, "bytes=%zu|", body.size());
    LogLine(id_, "act.serve_html", std::string(t) + "hdrs=" + HeaderDump(r));
    return RQ_NOTIFICATION_FINISH_REQUEST;
  }

  const ModInfo* info_;
  std::string id_ = "-";
  long in_send_ = 0;
  long sends_ = 0;
  bool served_ = false;
  bool rewrote_ = false;
  bool capture_has_headers_ = false;
  bool capture_failed_ = false;
  std::string capture_hdrs_;
  std::string capture_body_;
};

class ProbeFactory : public IHttpModuleFactory {
 public:
  explicit ProbeFactory(const ModInfo* info) : info_(info) {}
  HRESULT GetHttpModule(CHttpModule** module, IModuleAllocator*) override {
    *module = new Probe(info_);
    return S_OK;
  }
  void Terminate() override { delete this; }

 private:
  const ModInfo* info_;
};

}  // namespace

extern "C" __declspec(dllexport) HRESULT __stdcall RegisterModule(
    DWORD, IHttpModuleRegistrationInfo* info, IHttpServer* server) {
  InitOnceExecuteOnce(&g_log_once, InitLogLock, nullptr, nullptr);
  g_server = server;
  // Never freed: it lives as long as the registration.
  ModInfo* mod = new ModInfo();
  _snprintf_s(mod->name, sizeof(mod->name), _TRUNCATE, "%ls", info->GetName());
  size_t n = strlen(mod->name);
  mod->letter = n > 0 ? mod->name[n - 1] : '?';
  // F, G and H register the send notification at the other end of the
  // order.
  mod->send_first = (mod->letter == 'F' || mod->letter == 'G' || mod->letter == 'H');
  mod->id = info->GetId();
  if (mod->letter >= 'A' && mod->letter <= 'Z') g_ids[mod->letter - 'A'] = mod->id;
  t_mod = mod;
  CreateDirectoryA("C:\\pipeprobe", nullptr);
  CreateDirectoryA("C:\\pipeprobe\\log", nullptr);
  HRESULT hr = info->SetRequestNotifications(
      new ProbeFactory(mod),
      RQ_BEGIN_REQUEST | RQ_RESOLVE_REQUEST_CACHE | RQ_SEND_RESPONSE, 0);
  if (FAILED(hr)) return hr;
  hr = info->SetPriorityForRequestNotification(RQ_RESOLVE_REQUEST_CACHE,
                                               PRIORITY_ALIAS_FIRST);
  if (FAILED(hr)) return hr;
  hr = info->SetPriorityForRequestNotification(
      RQ_SEND_RESPONSE,
      mod->send_first ? PRIORITY_ALIAS_FIRST : PRIORITY_ALIAS_LAST);
  if (FAILED(hr)) return hr;
  char image[MAX_PATH] = "?";
  HMODULE self = nullptr;
  if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                             GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCSTR>(&RegisterModule), &self)) {
    GetModuleFileNameA(self, image, MAX_PATH);
  }
  LogLine("-", "register",
          std::string(mod->send_first ? "send=first" : "send=last") +
              "|image=" + image);
  return S_OK;
}
