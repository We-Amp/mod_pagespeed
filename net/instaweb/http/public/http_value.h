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

#ifndef NET_INSTAWEB_HTTP_PUBLIC_HTTP_VALUE_H_
#define NET_INSTAWEB_HTTP_PUBLIC_HTTP_VALUE_H_

#include <cstddef>  // for size_t

#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/mapped_shared_string.h"
#include "pagespeed/kernel/base/shared_string.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/base/writer.h"

namespace net_instaweb {

class ResponseHeaders;
class MessageHandler;

// Provides shared, ref-counted, copy-on-write storage for HTTP
// contents, to aid sharing between active fetches and filters, and
// the cache, which from which data may be evicted at any time.
class HTTPValue : public Writer {
 public:
  HTTPValue() : contents_size_(0) {}

  // Clears the value (both headers and content).  Also releases any
  // borrowed memory-mapped view (see LinkMapped).
  void Clear();

  // Is this HTTPValue empty
  bool Empty() const { return !mapped_active_ && storage_.empty(); }

  // Sets the HTTP headers for this value. This method may only
  // be called once and must be called before or after all of the
  // contents are set (using the streaming interface Write).
  //
  // If Clear() is called, then SetHeaders() can be called once again.
  //
  // Does NOT take ownership of headers.
  // A non-const pointer is required for the response headers so that
  // the cache fields can be updated if necessary.
  void SetHeaders(ResponseHeaders* headers);

  // Writes contents into the HTTPValue object.  Write can be called
  // multiple times to append more data, and can be called before
  // or after SetHeaders.  However, SetHeaders cannot be interleaved
  // in between calls to Write.
  bool Write(const StringPiece& str, MessageHandler* handler) override;
  bool Flush(MessageHandler* handler) override;

  // Retrieves the headers, returning false if empty.
  bool ExtractHeaders(ResponseHeaders* headers, MessageHandler* handler) const;

  // Retrieves the contents, returning false if empty.  Note that the
  // contents are only guaranteed valid as long as the HTTPValue
  // object is in scope.
  bool ExtractContents(StringPiece* str) const;

  // Zero-copy serve helper (CycloneZeroCopyServe).  If this value is a
  // borrowed memory-mapped view (is_mapped()), sets *str to the body
  // StringPiece aliasing the mapped region (identical bytes to
  // ExtractContents) and *keepalive to a copy of the underlying
  // MappedSharedString (a cheap refcount bump on the Cyclone read handle,
  // carrying the design record renew/force-wrap-deadline hooks).  The bytes in
  // *str remain valid as long as *keepalive (or any copy) is alive.
  // Returns false if not mapped or the entry is corrupt.
  bool ExtractMappedContents(StringPiece* str,
                             MappedSharedString* keepalive) const;

  // Tests whether this reference is the only active one to the string object.
  bool unique() const {
    return mapped_active_ ? mapped_storage_.unique() : storage_.unique();
  }

  // Assigns the storage of an HTTPValue based on the provided storage.  This
  // can be used for a cache Get.  Returns false if the string is not
  // well-formed.
  //
  // Extracts the headers into the provided ResponseHeaders buffer.
  bool Link(const SharedString& src, ResponseHeaders* headers,
            MessageHandler* handler);

  // Like Link(const SharedString&, ...) but accepts a MappedSharedString.
  // If 'src' is backed by memory-mapped storage (src.is_mapped()), this
  // HTTPValue becomes a borrowed, zero-copy view over the mapped bytes:
  // ExtractContents/ExtractHeaders read directly from the mapped region
  // without copying.
  //
  // LIFETIME RULE: a mapped HTTPValue borrows cache-owned memory whose
  // protection window is bounded (Cyclone lease pinning, the design record).  It must
  // be consumed within the cache-callback / request-serving scope.  Any
  // operation that lets the bytes escape into longer-lived objects --
  // share() (cache Put), Link(HTTPValue*) (e.g. Resource::LinkFallbackValue),
  // or a mutation (Write/SetHeaders) -- first collapses this value to owned
  // storage automatically.  The collapse retains a reference to the mapped
  // view until Clear()/destruction so that previously returned
  // ExtractContents StringPieces stay valid for the life of this object,
  // preserving the documented ExtractContents contract.
  //
  // If 'src' is not mapped this is exactly Link(src.ToOwned(), ...).
  bool LinkMapped(const MappedSharedString& src, ResponseHeaders* headers,
                  MessageHandler* handler);

  // Returns true while this HTTPValue reads directly from a borrowed
  // memory-mapped view (i.e. LinkMapped succeeded and no owned-collapse has
  // happened yet).
  bool is_mapped() const { return mapped_active_; }

  // Links two HTTPValues together, using the contents of 'src' and discarding
  // the contents of this.
  //
  // If 'src' is in mapped mode it is first collapsed to owned storage:
  // this HTTPValue's lifetime is not bounded by the cache-callback scope
  // that protects the mapped bytes.
  void Link(HTTPValue* src) {
    if (src != this) {
      src->CollapseToOwned();
      storage_ = src->storage_;  // SharedString links via assignment.
      contents_size_ = src->contents_size();
      mapped_active_ = false;
      mapped_storage_ = MappedSharedString();
    }
  }

  // Access the shared string, for insertion into a cache via Put.
  //
  // Collapses a mapped value to owned storage first: the returned
  // SharedString may outlive the mapped view's protection window (it is
  // shared into caches and Resources).
  const SharedString& share() {
    CollapseToOwned();
    return storage_;
  }

  size_t size() const {
    return mapped_active_ ? mapped_storage_.size() : storage_.size();
  }
  int64 contents_size() { return contents_size_; }

  // Useful functions for debugging. See http_value_explorer.
  // Convert from HTTPValue format to raw HTTP stream.
  static bool Decode(StringPiece encoded_value, GoogleString* http_string,
                     MessageHandler* handler);
  // Convert from raw HTTP stream  to HTTPValue format.
  static bool Encode(StringPiece http_string, GoogleString* encoded_value,
                     MessageHandler* handler);

 private:
  friend class HTTPValueTest;

  // Raw encoded bytes: the owned storage_ or, in mapped mode, the borrowed
  // mapped view.  All read paths dispatch through these two accessors.
  const char* raw_data() const {
    return mapped_active_ ? mapped_storage_.data() : storage_.data();
  }
  // int (not size_t) to preserve the signed comparisons the corrupt-entry
  // guards in ExtractHeaders/ExtractContents/ComputeContentsSize rely on
  // (SharedString::size() is int).
  int raw_size() const {
    return mapped_active_ ? static_cast<int>(mapped_storage_.size())
                          : storage_.size();
  }

  // Must be called with raw storage non-empty.
  char type_identifier() const { return *raw_data(); }

  unsigned int SizeOfFirstChunk() const;
  void SetSizeOfFirstChunk(unsigned int size);
  int64 ComputeContentsSize() const;

  // Disconnects this HTTPValue from other HTTPValues that may share the
  // underlying storage, allowing a new buffer.  Collapses a mapped view
  // to owned storage first.
  void CopyOnWrite();

  // If in mapped mode, copies the mapped bytes into owned storage_ and
  // leaves mapped mode.  Retains mapped_storage_ as a keep-alive reference
  // so previously extracted StringPieces (which point into the mapped
  // region) stay valid until Clear()/destruction.
  void CollapseToOwned();

  SharedString storage_;
  // Borrowed memory-mapped view (LinkMapped).  Reads dispatch to it while
  // mapped_active_ is true; after a collapse it is retained (inactive) as a
  // keep-alive reference for previously extracted StringPieces.
  MappedSharedString mapped_storage_;
  bool mapped_active_ = false;
  // Member variable to keep the size of body in storage.
  int64 contents_size_;

  HTTPValue(const HTTPValue&) = delete;
  HTTPValue& operator=(const HTTPValue&) = delete;
};

}  // namespace net_instaweb

#endif  // NET_INSTAWEB_HTTP_PUBLIC_HTTP_VALUE_H_
