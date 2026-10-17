// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef PAGESPEED_IIS_IIS_ENTITY_CHUNK_H_
#define PAGESPEED_IIS_IIS_ENTITY_CHUNK_H_

// Reading the entity chunks of a response that OnSendResponse hands to the
// rewriter. Split out of iis_http_module.cpp so the bounds rules can be unit
// tested against real file handles without the IIS runtime.

#ifndef _WINSOCKAPI_
#define _WINSOCKAPI_
#endif
#include <winsock2.h>
#include <windows.h>
#include <http.h>

namespace net_instaweb {

// True for the entity chunk types whose bytes can be read into memory here.
// Chunks served from the kernel fragment cache cannot be.
bool IsReadableEntityChunk(const HTTP_DATA_CHUNK& chunk);

// True when every one of the `count` chunks is readable.
bool AllEntityChunksReadable(const HTTP_DATA_CHUNK* chunks, int count);

// Resolves the number of bytes a HttpDataChunkFromFileHandle chunk covers,
// including a range that runs to the end of the file. Returns false when the
// length cannot be determined, when the range starts past the end of the
// file, or when it is larger than `max_length`. The chunk is not modified.
bool ResolveFileChunkLength(const HTTP_DATA_CHUNK& chunk, ULONGLONG max_length,
                            ULONGLONG* length);

// Reads at most `length` bytes of a file-handle chunk's range, starting at the
// range's offset, into `buf` (which holds `length` bytes). Stops early when
// the file ends; *bytes_read says how many bytes were stored. Never writes
// more than `length` bytes into `buf`.
HRESULT ReadFileChunkInto(const HTTP_DATA_CHUNK& chunk, char* buf,
                          ULONGLONG length, ULONGLONG* bytes_read);

}  // namespace net_instaweb

#endif  // PAGESPEED_IIS_IIS_ENTITY_CHUNK_H_
