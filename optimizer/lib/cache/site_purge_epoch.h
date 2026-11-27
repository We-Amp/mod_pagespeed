// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef PAGESPEED_LIB_CACHE_SITE_PURGE_EPOCH_H_
#define PAGESPEED_LIB_CACHE_SITE_PURGE_EPOCH_H_

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace pagespeed {

// The per-site purge epoch: the time, in Unix seconds, of the last global
// purge one site's front end observed.  A front end treats an entry it stored
// at or before the epoch as purged.
//
// WHERE IT LIVES, and why there.  One small file per site BESIDE the cache
// volume, never inside it: the volume is a set of circular logs whose
// reclamation (wrap, full directory buckets, chain carry-forward) has no
// ordering against the entries the epoch fences, so a record in the volume
// can be reclaimed while older entries survive.  A file is not reclaimed,
// and it outlives a front end's own caches and processes.
//
//   <volume_path>.purge-epoch.<site>
//
// `site` is exactly 64 lowercase hex characters (it becomes part of a file
// name; anything else is refused before any path is built).
//
// FORMAT: one fixed-width ASCII record, "pspe1 " + 20 zero-padded decimal
// digits + "\n" (27 bytes), and nothing else.  An empty file (created, not
// yet written, or its creator died first) reads as no file: nothing was
// recorded.  Any other content — torn, short, longer, a format this build
// does not know — is kCorrupted, never 0.
//
// THE FILE IS UPDATED IN PLACE, never replaced.  A writer opens it (creating
// it 0660 if absent; the setgid cache directory gives it the cache group),
// takes an exclusive OS advisory lock (fcntl F_SETLK on POSIX, LockFileEx on
// Windows, on one byte far past the record so Windows' mandatory byte-range
// locks never block the record's own I/O), reads the record, and only for a
// larger value writes the whole fixed-width record at offset 0 and flushes
// it.  No directory entry is created, renamed or removed after the first
// write, so any user with write access to the file — any user of the cache
// group — can update it, including in a sticky directory owned by another
// user.  A reader takes a shared lock on the same byte, so it never sees a
// half-written record from a live writer.
//
// TORN WRITES AND THE CLOCK (a tear needs a power loss mid-write; a killed
// process cannot tear a 27-byte write).  A tear of the FIRST write, or of a repair of a longer file, leaves
// a short or mixed record that reads kCorrupted (fail closed); an empty file
// reads 0, since nothing was acknowledged.  A tear of an OVERWRITE leaves the
// new value's leading digits over the old value's trailing ones: still a
// well-formed record, and because both are zero-padded to one width and the
// new value is larger, the mix is never below the old value, so nothing is
// un-purged.  The epoch NEVER decreases: a stored value in the future — such
// a mixed record, or one written while the clock ran fast and then stepped
// back — is a valid value.  Readers return it and writers keep the larger of
// it and theirs, so it over-fences (fail closed) until the clock passes it,
// by design.  Lowering it would un-purge entries stored under the fast clock
// once real time passes their stored time.  The next write repairs a
// kCorrupted record with the larger of its value and the current time.  A
// process-wide mutex serialises this process's own uses of the file (fcntl
// locks belong to the process, and closing any descriptor drops them).
//
// Lock waits are bounded (about 2 s, retried every 5 ms); a lock that cannot
// be taken in time is kIo.
//
// A value at or below the stored one is a successful no-op.  A value later
// than `now_seconds` + kSitePurgeEpochSkewSeconds is kInvalidArgument.
//
// ANY error from a read — kCorrupted, kIo, anything — means the caller must
// treat every entry of the site as purged.  Only a successful read is a
// value; a missing file is the successful value 0.
//
// THE VOLUME GENERATION.  The file is not keyed by the volume's generation,
// on purpose.  When the volume is recreated it holds no entry from before,
// and the epoch only ever fences entries stored at or before it, so a file
// that outlives a recreate can only fence MORE than needed (fail closed,
// healed by the next store), never resurrect an entry; a file that is
// missing after a recreate fences nothing the new volume holds.  Keying by
// generation would instead drop the epoch whenever the generation moves for
// a reason other than a recreate.
enum class SitePurgeEpochError : std::uint8_t {
  kInvalidArgument,  // bad site, or a value too far in the future
  kIo,               // the file or its lock could not be read or written
  kCorrupted,        // the file exists and is not a value this build wrote
};

inline constexpr uint32_t kSitePurgeEpochSkewSeconds = 5;

// True for exactly 64 characters of [0-9a-f].
bool IsSitePurgeEpochSite(std::string_view site);

// The value file's path; empty when `site` is not a site id or
// `volume_path` is empty.
std::string SitePurgeEpochPath(std::string_view volume_path,
                               std::string_view site);

// 0 when the file does not exist.  A stored value in the future is returned
// as it is (see TORN WRITES AND THE CLOCK).
std::expected<uint64_t, SitePurgeEpochError> ReadSitePurgeEpochFile(
    std::string_view volume_path, std::string_view site);

std::expected<void, SitePurgeEpochError> WriteSitePurgeEpochFile(
    std::string_view volume_path, std::string_view site, uint64_t epoch_seconds,
    uint64_t now_seconds);

// The record for `epoch_seconds`, and the parser of one; exposed for tests.
std::string RenderSitePurgeEpoch(uint64_t epoch_seconds);
std::expected<uint64_t, SitePurgeEpochError> ParseSitePurgeEpoch(
    std::string_view content);

}  // namespace pagespeed

#endif  // PAGESPEED_LIB_CACHE_SITE_PURGE_EPOCH_H_
