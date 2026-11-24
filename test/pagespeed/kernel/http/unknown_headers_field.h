// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

// Test helpers for two kinds of field in the serialized response-headers
// message.
//
// An unknown field: one this build does not define, as a newer release might
// write it.  The headers are stored as a serialized HttpResponseHeaders
// message; the helpers attach a field with a tag the message does not use
// (99) by appending its bytes to an object's own binary form and reading
// that back, and tell whether an object still carries it.  This is how a
// build sees a field added after it was released.
//
// The size record: the field this build does define for the sizes of a
// rewritten output (ResponseHeaders::SizeRecord).

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

// The field on the wire.  Tag 99, wire type 2 (length-delimited): the key
// (99 << 3) | 2 = 794 as a varint is 0x9a 0x06, then the length (11) and the
// payload.
inline GoogleString UnknownHeadersFieldWire() {
  static const char kWire[] =
      "\x9a\x06\x0b"
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

// The two kinds of field a parameterized suite runs with: one this build does
// not know, standing in for a field added by a later release, and the size
// record, which this build defines.
enum StoredFieldKind { kUnknownField, kSizeRecordField };

// The size record the tests attach: distinct values in every field.
inline ResponseHeaders::SizeRecord TestSizeRecord() {
  ResponseHeaders::SizeRecord record;
  record.original_size = 123456;
  record.optimized_size = 7890;
  record.filter_id = "ic";
  return record;
}

inline void AttachSizeRecord(ResponseHeaders* headers) {
  headers->SetSizeRecord(TestSizeRecord());
}

// True if 'headers' carry exactly the record AttachSizeRecord sets.
inline bool CarriesSizeRecord(const ResponseHeaders& headers) {
  const ResponseHeaders::SizeRecord expected = TestSizeRecord();
  ResponseHeaders::SizeRecord record;
  return headers.GetSizeRecord(&record) &&
         record.original_size == expected.original_size &&
         record.optimized_size == expected.optimized_size &&
         record.filter_id == expected.filter_id;
}

}  // namespace net_instaweb

#endif  // TEST_PAGESPEED_KERNEL_HTTP_UNKNOWN_HEADERS_FIELD_H_
