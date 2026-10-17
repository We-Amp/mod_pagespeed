// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "pagespeed/iis/iis_entity_chunk.h"

#include <cstring>

namespace net_instaweb {

namespace {

// Reads are page aligned: the file handle may have been opened for
// unbuffered, overlapped I/O.
constexpr DWORD kIoPageSize = 4096;

}  // namespace

bool IsReadableEntityChunk(const HTTP_DATA_CHUNK& chunk) {
  return chunk.DataChunkType == HttpDataChunkFromMemory ||
         chunk.DataChunkType == HttpDataChunkFromFileHandle;
}

bool AllEntityChunksReadable(const HTTP_DATA_CHUNK* chunks, int count) {
  for (int i = 0; i < count; ++i) {
    if (!IsReadableEntityChunk(chunks[i])) {
      return false;
    }
  }
  return true;
}

bool ResolveFileChunkLength(const HTTP_DATA_CHUNK& chunk, ULONGLONG max_length,
                            ULONGLONG* length) {
  const HTTP_BYTE_RANGE& range = chunk.FromFileHandle.ByteRange;
  ULONGLONG resolved = range.Length.QuadPart;
  if (resolved == HTTP_BYTE_RANGE_TO_EOF) {
    HANDLE file = chunk.FromFileHandle.FileHandle;
    LARGE_INTEGER file_size;
    if (GetFileType(file) != FILE_TYPE_DISK ||
        !GetFileSizeEx(file, &file_size) || file_size.QuadPart < 0) {
      return false;
    }
    ULONGLONG size = static_cast<ULONGLONG>(file_size.QuadPart);
    if (range.StartingOffset.QuadPart > size) {
      return false;
    }
    resolved = size - range.StartingOffset.QuadPart;
  }
  if (resolved > max_length) {
    return false;
  }
  *length = resolved;
  return true;
}

HRESULT ReadFileChunkInto(const HTTP_DATA_CHUNK& chunk, char* buf,
                          ULONGLONG length, ULONGLONG* bytes_read) {
  *bytes_read = 0;
  HANDLE file = chunk.FromFileHandle.FileHandle;
  const ULONGLONG start = chunk.FromFileHandle.ByteRange.StartingOffset.QuadPart;
  if (length == 0) {
    return S_OK;
  }
  if (start > ~0ULL - length) {
    return E_INVALIDARG;
  }

  BYTE* page = static_cast<BYTE*>(VirtualAlloc(
      nullptr, kIoPageSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
  if (page == nullptr) {
    return HRESULT_FROM_WIN32(GetLastError());
  }
  HANDLE io_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
  if (io_event == nullptr) {
    HRESULT hr = HRESULT_FROM_WIN32(GetLastError());
    VirtualFree(page, 0, MEM_RELEASE);
    return hr;
  }

  HRESULT hr = S_OK;
  ULONGLONG done = 0;
  while (done < length) {
    const ULONGLONG offset = start + done;
    const ULONGLONG aligned = offset & ~static_cast<ULONGLONG>(kIoPageSize - 1);
    const DWORD skip = static_cast<DWORD>(offset - aligned);

    OVERLAPPED ovl;
    ZeroMemory(&ovl, sizeof(ovl));
    ovl.hEvent = io_event;
    ovl.Offset = static_cast<DWORD>(aligned);
    ovl.OffsetHigh = static_cast<DWORD>(aligned >> 32);

    DWORD got = 0;
    bool at_eof = false;
    if (!ReadFile(file, page, kIoPageSize, &got, &ovl)) {
      DWORD error = GetLastError();
      if (error == ERROR_IO_PENDING) {
        // Wait on the event ourselves: GetOverlappedResult can return without
        // waiting and leave the auto-reset event signalled for the next read.
        WaitForSingleObject(io_event, INFINITE);
        if (!GetOverlappedResult(file, &ovl, &got, TRUE)) {
          error = GetLastError();
          if (error != ERROR_HANDLE_EOF) {
            hr = HRESULT_FROM_WIN32(error);
            break;
          }
          at_eof = true;
        }
      } else if (error == ERROR_HANDLE_EOF) {
        at_eof = true;
      } else {
        hr = HRESULT_FROM_WIN32(error);
        break;
      }
    }
    if (got > kIoPageSize) {
      hr = E_UNEXPECTED;
      break;
    }
    // The file ends at or before the offset this page read was meant to
    // continue from: the file is shorter than the range.
    if (at_eof || got <= skip) {
      break;
    }
    ULONGLONG n = got - skip;
    if (n > length - done) {
      n = length - done;
    }
    std::memcpy(buf + done, page + skip, static_cast<size_t>(n));
    done += n;
  }

  CloseHandle(io_event);
  VirtualFree(page, 0, MEM_RELEASE);
  *bytes_read = done;
  return hr;
}

}  // namespace net_instaweb
