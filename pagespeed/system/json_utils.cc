// Copyright (c) 2024-2026 We-Amp B.V.

#include "pagespeed/system/json_utils.h"

#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"

namespace net_instaweb {

// NOTE: JsonEscape() now lives in pagespeed/kernel/base/string_util.h
// (inline, canonical source from pagespeed-optimizer).

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

  // Read until closing quote, handling escaped characters.
  GoogleString result;
  while (pos < json.size()) {
    if (json[pos] == '\\' && pos + 1 < json.size()) {
      char next = json[pos + 1];
      switch (next) {
        case '"':
          result += '"';
          break;
        case '\\':
          result += '\\';
          break;
        case 'n':
          result += '\n';
          break;
        case 'r':
          result += '\r';
          break;
        case 't':
          result += '\t';
          break;
        case '/':
          result += '/';
          break;
        default:
          result += '\\';
          result += next;
          break;
      }
      pos += 2;
      continue;
    }
    if (json[pos] == '"') break;
    result += json[pos];
    ++pos;
  }
  *value = result;
  return true;
}

bool ExtractJsonBoolField(StringPiece json, StringPiece field_name,
                          bool* value) {
  GoogleString search = StrCat("\"", field_name, "\"");
  StringPiece::size_type pos = json.find(search);
  if (pos == StringPiece::npos) return false;
  pos += search.size();

  // Skip whitespace and colon.
  while (pos < json.size() && (json[pos] == ' ' || json[pos] == ':')) ++pos;
  if (pos >= json.size()) return false;

  if (json.substr(pos, 4) == "true") {
    *value = true;
    return true;
  }
  if (json.substr(pos, 5) == "false") {
    *value = false;
    return true;
  }
  return false;
}

}  // namespace net_instaweb
