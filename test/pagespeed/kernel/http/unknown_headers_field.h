// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

// Test helpers for a field of the serialized response-headers message that
// this build does not define, as a newer release might write it.  The headers
// are stored as a serialized HttpResponseHeaders message; tag 15 is the next
// free one.  The helpers attach such a field to a ResponseHeaders object by
// appending its bytes to the object's own binary form and reading that back,
// and tell whether an object still carries it.

#ifndef TEST_PAGESPEED_KERNEL_HTTP_UNKNOWN_HEADERS_FIELD_H_
#define TEST_PAGESPEED_KERNEL_HTTP_UNKNOWN_HEADERS_FIELD_H_

#include "pagespeed/kernel/base/google_message_handler.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/base/string_writer.h"
#include "pagespeed/kernel/http/response_headers.h"
#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {

// The payload of the unknown field; it must not show up in any text form.
inline const char kUnknownHeadersFieldPayload[] = "size-record";

// The field on the wire.  Tag 15, wire type 2 (length-delimited): key byte
// (15 << 3) | 2 = 0x7a, then the length (11) and the payload.
inline GoogleString UnknownHeadersFieldWire() {
  static const char kWire[] =
      "\x7a\x0b"
      "size-record";
  return GoogleString(kWire, STATIC_STRLEN(kWire));
}

inline GoogleString SerializeHeaders(ResponseHeaders* headers) {
  GoogleString wire;
  StringWriter writer(&wire);
  GoogleMessageHandler handler;
  EXPECT_TRUE(headers->WriteAsBinary(&writer, &handler));
  return wire;
}

// True if the binary form of 'headers' holds the unknown field, byte for byte.
inline bool CarriesUnknownField(ResponseHeaders* headers) {
  return SerializeHeaders(headers).find(UnknownHeadersFieldWire()) !=
         GoogleString::npos;
}

// Re-reads 'headers' from their own binary form with the unknown field
// appended, the way an entry written by a newer release would be read.
inline void AttachUnknownField(ResponseHeaders* headers) {
  GoogleString wire = SerializeHeaders(headers);
  wire.append(UnknownHeadersFieldWire());
  GoogleMessageHandler handler;
  ASSERT_TRUE(headers->ReadFromBinary(wire, &handler));
}

}  // namespace net_instaweb

#endif  // TEST_PAGESPEED_KERNEL_HTTP_UNKNOWN_HEADERS_FIELD_H_
