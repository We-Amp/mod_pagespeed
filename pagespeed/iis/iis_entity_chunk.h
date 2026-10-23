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
// clang-format off
// Order matters: winsock2.h must come before windows.h, and http.h needs both.
#include <winsock2.h>
#include <windows.h>
#include <http.h>
// clang-format on

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

// Allocates `size` bytes that stay valid for as long as the response (the
// request memory pool). Returns null on failure.
using EntityChunkAllocator = void* (*)(void* context, DWORD size);

// Reads every HttpDataChunkFromFileHandle chunk among the `count` chunks into
// memory obtained from `allocate`, each bounded by `max_length` bytes. Only
// when every one of them was read in full are they turned into
// HttpDataChunkFromMemory chunks holding those bytes; otherwise returns false
// and leaves all chunks unchanged, so the response can still be sent as is.
bool ConvertFileChunksToMemory(HTTP_DATA_CHUNK* chunks, int count,
                               ULONGLONG max_length,
                               EntityChunkAllocator allocate, void* context);

}  // namespace net_instaweb

#endif  // PAGESPEED_IIS_IIS_ENTITY_CHUNK_H_
