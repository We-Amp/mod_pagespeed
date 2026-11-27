# Multi-Process Cache Stripe Interaction

mod_pagespeed uses the Cyclone Cache library with a specific multi-process
configuration. This document describes how nginx and the worker daemon share
a single cache file safely and efficiently.

## Two Processes, One Cache File

Two separate processes share a single on-disk cache volume:

1. **Nginx** (the web server; its worker user is a member of group `pagespeed`) — thin cache interceptor
2. **Worker** (the optimizer worker, runs as the unprivileged `pagespeed` user) — does optimization

They both open the same `.vol` file, which the worker creates with mode `0660` (owner `pagespeed`, shared with the web server through group `pagespeed`).

## Stripe Affinity — Deliberately Bypassed

Cyclone supports full multi-process stripe affinity where
`stripe_owner = stripe_index % total_processes` and only the owner can write.
**mod_pagespeed does not use this.** Both processes open the cache with:

```cpp
cyclone::MultiProcessConfig multi_process = {
    .enabled = true,
    .process_index = 0,
    .total_processes = 1,
};
```

With `total_processes=1`, all stripes are owned by both processes — no write
rejection occurs. Multi-process mode is enabled solely to activate the
mmap-backed directory.

## Cache Write Invariant (Application-Level Partitioning)

Instead of stripe-level ownership, write safety comes from an application-level
convention:

- **Nginx only writes the default-mask alternate** (`CapabilityMask()` = 0x08 —
  Desktop/Identity/Original content).
- **Worker writes all other alternates** (WebP, AVIF, SVG, compressed variants,
  early hints, etc.).

Since they write to different `AlternateId`s for the same cache key, there is no
write contention by design.

## Mmap Directory = Cross-Process Visibility

The critical mechanism enabling cross-process sharing is the mmap-backed
directory (`enable_mmap_directory = true`):

- The directory (mapping cache keys to stripe offsets) is stored **in the cache
  file itself** via memory-mapped I/O.
- When nginx writes an original, the worker sees it immediately through the
  shared mmap.
- When the worker writes an optimized variant, nginx sees it on the next read.
- No IPC is needed for directory lookups — just memory-mapped shared state.

> **WARNING**: Both nginx and worker MUST open cache with
> `enable_mmap_directory = true`. Without it, worker writes are invisible
> to nginx. Symptom: `X-PageSpeed: HIT` but content is original (not optimized).

## Torn Read Safety

Since both processes can read and write simultaneously, Cyclone protects the
**read call itself** with two layers:

1. **Seqlock on directory buckets** — a version counter is checked before and
   after reading a directory entry. A mismatch triggers a retry (up to
   `kMaxReadRetries = 10`).
2. **CRC32 on data** — if a read overlaps with an in-progress write, the
   checksum fails and the read is treated as a cache miss (not corruption).

For the read call this gives eventual consistency: a torn read results in a
temporary miss, never corrupted data. **These layers cover only the moment of
the read.** A `ReadHandle` returned by that read is a borrow of the mmap'd
bytes, held long after the CRC check ran — nginx parks it on the request
context until request cleanup, and the worker holds it across optimization
pipelines. Nothing about the seqlock or the CRC protects that borrow window.

## Borrow Lifetime Safety — Read Leases

The borrow window is protected by lease-based region pinning:

- **Stamp**: every disk-borrow read stamps a per-stripe lease of
  `read_lease_duration` (default T = 5s). RAM-cache hits never stamp
  (their bytes are process-local copies, not borrows).
- **Wrap deferral**: a writer that would wrap the stripe's circular write
  buffer over leased bytes defers the wrap (dropping that fill) while an
  unexpired lease exists — the borrowed bytes are never overwritten in
  place under a live handle.
- **Renewal obligation**: the stamp-avoidance guard makes **3T/4 (3.75s)
  the guaranteed floor**. Any holder keeping a borrow longer must call
  `ReadHandle::renew_lease()` (surfaced as `ReadResult::renew_lease()` /
  `ps_read_renew_lease()`) at a cadence ≤ 3T/4, or copy the bytes.
  In this repo: nginx re-stamps from a per-request 3s timer while a
  ReadResult is parked on the request ctx; the .NET middleware re-stamps
  between write chunks; the worker re-stamps per transcode iteration.
- **Ceiling caveat**: wrap deferral is bounded by an anti-starvation
  ceiling (`lease_wrap_ceiling`, default 60s). Past it the wrap is FORCED
  (counted in the `wraps_forced_past_lease` stat) and the borrow is
  unprotected again — the same exposure as pre-lease behavior, now
  bounded and observable. Holds that must exceed the ceiling must copy.
- `read_lease_duration = 0` disables leases (pre-lease behavior).

## RAM Cache Considerations

Each process has its own in-process RAM cache. Because RAM is per-process:

- **Write-around policy**: writes do NOT populate the RAM cache. Only reads
  populate it on miss. This prevents stale-RAM scenarios (e.g., nginx's RAM
  cache holds the original while the worker has already written an optimized
  variant to disk).
- **Targeted eviction** (`EvictRamCache`) is used when nginx detects a
  `kFlagNeedsRevalidation` alternate — it evicts from RAM so the next read hits
  disk and picks up the worker's re-processed version.

## End-to-End Flow

```
nginx writes original (AlternateId 0x08) → disk stripe → mmap directory updated
    ↓
worker reads original via mmap directory → optimizes → writes variants
    ↓
nginx reads optimized variant via mmap directory → zero-copy serve via mmap'd data
```

Both processes own all stripes (`total_processes=1`). Contention is avoided by
the alternate-ID partitioning convention. Cross-process visibility comes from the
mmap-backed directory.

The zero-copy serve in the last step is safe for the duration of the transfer
because the read lease pins the stripe against wraps (with the ≤ 3T/4 renewal
obligation above), **up to the 60s ceiling** — a transfer that keeps the borrow
past the ceiling is unprotected for its remainder. Note the sendfile serve path
reads the volume file directly at the content's file offset: those bytes are
never CRC-checked at serve time, so the lease is the *only* protection on that
path.

## File Permissions

- Cache volume file: `0660`, owner `pagespeed`, group `pagespeed`; the web server reaches it as a group member (set with an explicit `fchmod`, never from the umask)
- Notification, health and management sockets: `0660`, same owner and group
- Shared config: `0640`; serve-stats file: `0660`
