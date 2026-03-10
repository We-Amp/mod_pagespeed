// Copyright (c) 2024-2026 We-Amp B.V.

#include "pagespeed/system/admin_license_handler.h"

#include "net/instaweb/http/public/async_fetch.h"
#include "pagespeed/kernel/base/message_handler.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/base/string_writer.h"
#include "pagespeed/kernel/base/timer.h"
#include "pagespeed/kernel/http/content_type.h"
#include "pagespeed/kernel/http/http_names.h"
#include "pagespeed/kernel/http/response_headers.h"
#include "pagespeed/kernel/license_v2/license_file.h"
#include "pagespeed/kernel/license_v2/license_token.h"
#include "pagespeed/kernel/license_v2/license_verifier.h"

namespace net_instaweb {

namespace {

// Simple JSON string escaping.
GoogleString JsonEscape(StringPiece s) {
  GoogleString result;
  result.reserve(s.size() + 10);
  for (size_t i = 0; i < s.size(); ++i) {
    char c = s[i];
    switch (c) {
      case '"':  result += "\\\""; break;
      case '\\': result += "\\\\"; break;
      case '\n': result += "\\n"; break;
      case '\r': result += "\\r"; break;
      case '\t': result += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          snprintf(buf, sizeof(buf), "\\u%04x",
                   static_cast<unsigned char>(c));
          result += buf;
        } else {
          result += c;
        }
    }
  }
  return result;
}

// Extract a JSON string field from a simple JSON object.
// This is a minimal parser; we don't pull in a full JSON library.
bool ExtractJsonStringField(StringPiece json, StringPiece field_name,
                            GoogleString* value) {
  GoogleString search = StrCat("\"", field_name, "\"");
  StringPiece::size_type pos = json.find(search);
  if (pos == StringPiece::npos) return false;
  pos += search.size();

  // Skip whitespace and colon.
  while (pos < json.size() && (json[pos] == ' ' || json[pos] == ':')) ++pos;
  if (pos >= json.size() || json[pos] != '"') return false;
  ++pos;

  // Read until closing quote (simple — no escaped quotes in field values).
  GoogleString result;
  while (pos < json.size() && json[pos] != '"') {
    result += json[pos];
    ++pos;
  }
  *value = result;
  return true;
}

}  // namespace

AdminLicenseHandler::AdminLicenseHandler(Timer* timer, MessageHandler* handler)
    : timer_(timer), handler_(handler) {}

void AdminLicenseHandler::Init() {
  // Try to read license from the default file path.
  GoogleString token;
  std::filesystem::path path = LicenseFilePath();
  if (ReadLicenseFile(path, &token)) {
    GoogleString error;
    if (ApplyToken(token, &error)) {
      handler_->Message(kInfo, "License loaded from %s",
                        path.string().c_str());
    } else {
      handler_->Message(kWarning, "Invalid license in %s: %s",
                        path.string().c_str(), error.c_str());
    }
  }
}

bool AdminLicenseHandler::IsLicenseValid() const {
  return license_valid_ && !license_expired_;
}

bool AdminLicenseHandler::ApplyToken(StringPiece token, GoogleString* error) {
  LicenseResult result = VerifyLicenseToken(token);
  if (!result.valid) {
    *error = result.error;
    return false;
  }

  token.CopyToString(&license_token_);
  license_valid_ = true;
  license_expired_ = result.expired;
  license_expires_at_ = result.expires_at;
  license_plan_ = result.payload.plan;
  license_sub_ = result.payload.sub;
  license_iat_ = result.payload.iat;
  return true;
}

bool AdminLicenseHandler::HandleRequest(StringPiece path,
                                         StringPiece request_body,
                                         AsyncFetch* fetch) {
  if (path == "/v1/license/status") {
    HandleStatus(fetch);
    return true;
  }
  if (path == "/v1/license/apply") {
    HandleApply(request_body, fetch);
    return true;
  }
  return false;
}

void AdminLicenseHandler::HandleStatus(AsyncFetch* fetch) {
  GoogleString json = "{";
  StrAppend(&json, "\"licensed\":", license_valid_ ? "true" : "false");
  if (license_valid_) {
    if (!license_plan_.empty()) {
      StrAppend(&json, ",\"license_type\":\"", JsonEscape(license_plan_), "\"");
    }
    if (license_expires_at_ > 0) {
      StrAppend(&json, ",\"expires\":", Integer64ToString(license_expires_at_));
    }
    if (license_expired_) {
      StrAppend(&json, ",\"expired\":true");
    }
    if (!license_sub_.empty()) {
      StrAppend(&json, ",\"domain\":\"", JsonEscape(license_sub_), "\"");
    }
  }
  // Trial is always available if not licensed.
  if (!license_valid_) {
    StrAppend(&json, ",\"trial_available\":true");
  }
  StrAppend(&json, "}");
  WriteJsonResponse(fetch, json);
}

void AdminLicenseHandler::HandleApply(StringPiece request_body,
                                       AsyncFetch* fetch) {
  if (request_body.size() > 4096) {
    WriteJsonError(fetch, HttpStatus::kBadRequest,
                   "Request body too large (max 4KB)");
    return;
  }

  // Extract the license key from the JSON body.
  GoogleString key;
  if (!ExtractJsonStringField(request_body, "key", &key) &&
      !ExtractJsonStringField(request_body, "license_key", &key)) {
    WriteJsonError(fetch, HttpStatus::kBadRequest,
                   "Missing 'key' or 'license_key' field");
    return;
  }

  if (key.empty()) {
    WriteJsonError(fetch, HttpStatus::kBadRequest,
                   "License key must not be empty");
    return;
  }

  GoogleString error;
  if (!ApplyToken(key, &error)) {
    GoogleString json = StrCat("{\"success\":false,\"error\":\"",
                               JsonEscape(error), "\"}");
    WriteJsonResponse(fetch, json);
    return;
  }

  // Persist to disk (best-effort).
  std::filesystem::path path = LicenseFilePath();
  if (WriteLicenseFile(path, key)) {
    handler_->Message(kInfo, "License saved to %s", path.string().c_str());
  } else {
    handler_->Message(kWarning, "Failed to save license to %s",
                      path.string().c_str());
  }

  GoogleString json = "{\"success\":true,\"message\":\"License applied\"";
  if (!license_plan_.empty()) {
    StrAppend(&json, ",\"plan\":\"", JsonEscape(license_plan_), "\"");
  }
  if (license_expires_at_ > 0) {
    StrAppend(&json, ",\"expires_at\":",
              Integer64ToString(license_expires_at_));
  }
  StrAppend(&json, "}");
  WriteJsonResponse(fetch, json);
}

void AdminLicenseHandler::WriteJsonResponse(AsyncFetch* fetch,
                                             StringPiece json) {
  ResponseHeaders* headers = fetch->response_headers();
  headers->SetStatusAndReason(HttpStatus::kOK);
  headers->Add(HttpAttributes::kContentType, kContentTypeJson.mime_type());
  int64 now_ms = timer_->NowMs();
  headers->SetLastModified(now_ms);
  fetch->Write(json, handler_);
  fetch->Done(true);
}

void AdminLicenseHandler::WriteJsonError(AsyncFetch* fetch, int status_code,
                                          StringPiece error) {
  ResponseHeaders* headers = fetch->response_headers();
  headers->set_status_code(status_code);
  headers->Add(HttpAttributes::kContentType, kContentTypeJson.mime_type());
  GoogleString json = StrCat("{\"error\":\"", JsonEscape(error), "\"}");
  fetch->Write(json, handler_);
  fetch->Done(true);
}

}  // namespace net_instaweb
