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

#include "net/instaweb/http/public/http_value.h"

#include <limits>

#include "base/logging.h"
#include "pagespeed/kernel/base/shared_string.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/base/string_writer.h"
#include "pagespeed/kernel/http/response_headers.h"
#include "pagespeed/kernel/http/response_headers_parser.h"

namespace {

// The headers and body are both encoded into one Shared String, which can then
// be efficiently held in an in-memory cache, or passed around as an HTTPValue
// object.  The class supports both setting the headers first and then the body,
// and vice versa.  Both the headers and body are variable length, and to avoid
// having to re-shuffle memory, we encode which is first in the buffer as the
// first byte.  The next four bytes encode the size.
const char kHeadersFirst = 'h';
const char kBodyFirst = 'b';

const int kStorageTypeOverhead = 1;
const int kStorageSizeOverhead = 4;
const int kStorageOverhead = kStorageTypeOverhead + kStorageSizeOverhead;

}  // namespace

namespace net_instaweb {

class MessageHandler;

void HTTPValue::CopyOnWrite() {
  CollapseToOwned();
  storage_.DetachRetainingContent();
}

void HTTPValue::CollapseToOwned() {
  if (!mapped_active_) {
    return;
  }
  StringPiece mapped = mapped_storage_.Value();
  storage_.DetachAndClear();
  storage_.Append(mapped.data(), mapped.size());
  // Leave mapped mode but retain mapped_storage_: previously extracted
  // StringPieces point into the mapped region and must stay valid for the
  // life of this HTTPValue (or until Clear()).
  //
  // the design record note -- this copy is deliberately NOT epoch-verified.  A
  // verified collapse would need a failure channel, and none exists
  // cleanly: (a) the API is void and its callers (share() during
  // OutputResource::Link, CopyOnWrite before any mutation) have no failure
  // semantics; (b) the only meaningful failure action, Clear(), would
  // violate the previously-extracted-StringPieces contract this function
  // exists to preserve (dangling pointers, a worse bug than the one being
  // guarded).  The residual window is small by construction: the collapse
  // runs within the same callback chain as the cache read, under the read
  // lease stamped at that read (normal wraps are blocked while it is
  // live); only a ceiling-forced wrap racing this ms-scale copy could tear
  // it -- the same accepted class as every other synchronous post-read
  // copy.  Serve paths needing the stronger guarantee de-alias via
  // ExtractMappedContents + CopyMappedVerified (or the port barrier)
  // BEFORE the value escapes, not via collapse.
  mapped_active_ = false;
}

void HTTPValue::Clear() {
  storage_.DetachAndClear();
  mapped_storage_ = MappedSharedString();
  mapped_active_ = false;
  contents_size_ = 0;
}

void HTTPValue::SetHeaders(ResponseHeaders* headers) {
  CopyOnWrite();
  GoogleString headers_string;
  StringWriter writer(&headers_string);
  headers->WriteAsBinary(&writer, nullptr);
  if (storage_.empty()) {
    storage_.Append(&kHeadersFirst, 1);
    SetSizeOfFirstChunk(headers_string.size());
  } else {
    CHECK(type_identifier() == kBodyFirst);
    // Using 'unsigned int' to facilitate bit-shifting in
    // SizeOfFirstChunk and SetSizeOfFirstChunk, and I don't
    // want to worry about sign extension.
    int size = SizeOfFirstChunk();
    CHECK_EQ(storage_.size(), (kStorageOverhead + size));
  }
  storage_.Append(headers_string);
}

bool HTTPValue::Write(const StringPiece& str, MessageHandler* handler) {
  CopyOnWrite();
  if (storage_.empty()) {
    // We have received data prior to receiving response headers.
    storage_.Append(&kBodyFirst, 1);
    SetSizeOfFirstChunk(str.size());
  } else if (type_identifier() == kBodyFirst) {
    CHECK(storage_.size() >= kStorageOverhead);
    int string_size = SizeOfFirstChunk();
    CHECK(string_size == storage_.size() - kStorageOverhead);
    SetSizeOfFirstChunk(str.size() + string_size);
  } else {
    CHECK(type_identifier() == kHeadersFirst);
  }
  storage_.Append(str.data(), str.size());
  contents_size_ += str.size();
  return true;
}

bool HTTPValue::Flush(MessageHandler* handler) { return true; }

// Encode the size of the first chunk, which is either the headers or body,
// depending on the order they are called.  Rather than trying to assume any
// particular alignment for casting between char* and int*, we just manually
// encode one byte at a time.
void HTTPValue::SetSizeOfFirstChunk(unsigned int size) {
  CHECK(!storage_.empty()) << "type encoding should already be in first byte";
  char size_buffer[4];
  size_buffer[0] = size & 0xff;
  size_buffer[1] = (size >> 8) & 0xff;
  size_buffer[2] = (size >> 16) & 0xff;
  size_buffer[3] = (size >> 24) & 0xff;
  storage_.Extend(1 + sizeof(size_buffer));
  storage_.WriteAt(1, size_buffer, sizeof(size_buffer));
}

// Decodes the size of the first chunk, which is either the headers or body,
// depending on the order they are called.  Rather than trying to assume any
// particular alignment for casting between char* and int*, we just manually
// decode one byte at a time.
unsigned int HTTPValue::SizeOfFirstChunk() const {
  CHECK(raw_size() >= kStorageOverhead);
  const unsigned char* size_buffer =
      reinterpret_cast<const unsigned char*>(raw_data() + 1);
  unsigned int size = size_buffer[0];
  size |= size_buffer[1] << 8;
  size |= size_buffer[2] << 16;
  size |= size_buffer[3] << 24;
  return size;
}

// Note that we avoid CHECK, and instead return false on error.  So if
// our cache gets corrupted (say) on disk, we just consider it an
// invalid entry rather than aborting the server.
bool HTTPValue::ExtractHeaders(ResponseHeaders* headers,
                               MessageHandler* handler) const {
  bool ret = false;
  headers->Clear();
  if (raw_size() >= kStorageOverhead) {
    char type_id = type_identifier();
    const char* start = raw_data() + kStorageOverhead;
    int size = SizeOfFirstChunk();
    if (size <= raw_size() - kStorageOverhead) {
      if (type_id == kBodyFirst) {
        start += size;
        size = raw_size() - size - kStorageOverhead;
        ret = true;
      } else {
        ret = (type_id == kHeadersFirst);
      }
      if (ret) {
        ret = headers->ReadFromBinary(StringPiece(start, size), handler);
      }
    }
  }
  return ret;
}

// Note that we avoid CHECK, and instead return false on error.  So if
// our cache gets corrupted (say) on disk, we just consider it an
// invalid entry rather than aborting the server.
bool HTTPValue::ExtractContents(StringPiece* val) const {
  bool ret = false;
  if (raw_size() >= kStorageOverhead) {
    char type_id = type_identifier();
    const char* start = raw_data() + kStorageOverhead;
    int size = SizeOfFirstChunk();
    if (size <= raw_size() - kStorageOverhead) {
      if (type_id == kHeadersFirst) {
        start += size;
        size = raw_size() - size - kStorageOverhead;
        ret = true;
      } else {
        ret = (type_id == kBodyFirst);
      }
      *val = StringPiece(start, size);
    }
  }
  return ret;
}

bool HTTPValue::ExtractMappedContents(StringPiece* val,
                                      MappedSharedString* keepalive) const {
  if (!mapped_active_) {
    return false;
  }
  if (!ExtractContents(val)) {
    return false;
  }
  // Reference copy: bumps the shared read-handle refcount, pinning the
  // mapped region for as long as any copy of *keepalive is alive.  No copy.
  *keepalive = mapped_storage_;
  return true;
}

int64 HTTPValue::ComputeContentsSize() const {
  // Return size as 0 if the cache is corrupted.
  int64 size = 0;
  if (raw_size() >= kStorageOverhead) {
    // Get the type id which is stored first (head or body).
    char type_id = type_identifier();
    // Get the size of the type which is stored first.
    size = SizeOfFirstChunk();
    // If the headers are stored first then update the size with storage size -
    // first chunk size.
    if ((size <= static_cast<int64>(raw_size() - kStorageOverhead)) &&
        (type_id == kHeadersFirst)) {
      size = raw_size() - size - kStorageOverhead;
    }
  }
  return size;
}

bool HTTPValue::Link(const SharedString& src, ResponseHeaders* headers,
                     MessageHandler* handler) {
  bool ok = false;
  if (src.size() >= kStorageOverhead) {
    // The simplest way to ensure that src is well formed is to save the
    // existing storage_ in a temp, assign the storage, and make sure
    // Headers and Contents return true.  The drawback is that the headers
    // parsing is arguably a little heavyweight.  We could consider encoding
    // the headers in an easier-to-extract form, so we don't have to give up
    // the integrity checks.
    SharedString temp(storage_);
    bool was_mapped = mapped_active_;
    int64 old_contents_size = contents_size_;
    storage_ = src;
    mapped_active_ = false;
    contents_size_ = ComputeContentsSize();

    // TODO(jmarantz): this could be a lot lighter weight, but we are going
    // to be sure at this point that both the headers and the contents are
    // valid.  It would be nice to have an HTML headers parser that didn't
    // actually create new temp copies of all the names/values.
    ok = ExtractHeaders(headers, handler);
    if (!ok) {
      storage_ = temp;
      mapped_active_ = was_mapped;
      contents_size_ = old_contents_size;
    } else {
      // The old mapped view (if any) has been replaced wholesale; drop the
      // keep-alive reference like a successful re-Link drops old storage.
      mapped_storage_ = MappedSharedString();
    }
  }
  return ok;
}

bool HTTPValue::LinkMapped(const MappedSharedString& src,
                           ResponseHeaders* headers, MessageHandler* handler) {
  if (!src.is_mapped()) {
    // Owned mode: exactly the classic Link (ToOwned() is a cheap
    // reference copy when the MappedSharedString is owned).
    return Link(src.ToOwned(), headers, handler);
  }
  bool ok = false;
  // raw_size() narrows the mapped length to int (to match SharedString::size()
  // and the signed corrupt-entry guards in ExtractHeaders/ExtractContents).
  // Reject a (hypothetical) > INT_MAX mapped entry as a clean miss rather than
  // let the narrowing wrap into a mis-parse.
  if (src.size() >= static_cast<size_t>(kStorageOverhead) &&
      src.size() <= static_cast<size_t>(std::numeric_limits<int>::max())) {
    // Validate exactly like Link(): swap the view in, make sure the headers
    // parse; restore the previous state on failure.
    SharedString old_storage(storage_);
    MappedSharedString old_mapped(mapped_storage_);
    bool was_mapped = mapped_active_;
    int64 old_contents_size = contents_size_;

    mapped_storage_ = src;  // Reference copy; no byte copy.
    mapped_active_ = true;
    contents_size_ = ComputeContentsSize();

    ok = ExtractHeaders(headers, handler);
    if (ok) {
      // Reads now come from the mapped view; owned storage is unused.
      storage_.DetachAndClear();
    } else {
      storage_ = old_storage;
      mapped_storage_ = old_mapped;
      mapped_active_ = was_mapped;
      contents_size_ = old_contents_size;
    }
  }
  return ok;
}

bool HTTPValue::Decode(StringPiece encoded_value, GoogleString* http_string,
                       MessageHandler* handler) {
  ResponseHeaders headers;

  // Load encoded value into an HTTPValue and extract headers.
  SharedString buffer(encoded_value);
  HTTPValue value;
  if (!value.Link(buffer, &headers, handler)) return false;

  // Extract decoded contents.
  StringPiece contents;
  if (!value.ExtractContents(&contents)) return false;

  // Return result as normal HTTP stream.
  *http_string = StrCat(headers.ToString(), contents);
  return true;
}

bool HTTPValue::Encode(StringPiece http_string, GoogleString* encoded_value,
                       MessageHandler* handler) {
  // Parse headers.
  ResponseHeaders headers;
  ResponseHeadersParser headers_parser(&headers);
  int bytes_parsed = headers_parser.ParseChunk(http_string, handler);
  if (!headers.headers_complete()) return false;

  // Rest is contents.
  StringPiece contents = http_string.substr(bytes_parsed);

  // Encode into HTTPValue.
  HTTPValue value;
  value.SetHeaders(&headers);
  value.Write(contents, handler);

  // Return SharedString buffer.
  *encoded_value = value.share().Value().as_string();
  return true;
}

}  // namespace net_instaweb
