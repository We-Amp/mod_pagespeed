# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")
load("@bazel_tools//tools/build_defs/repo:utils.bzl", "maybe")
load("@bazel_tools//tools/build_defs/repo:git.bzl", "git_repository")
load(":hiredis.bzl", "hiredis_build_rule")
load(":jsoncpp.bzl", "jsoncpp_build_rule")
load(":libpng.bzl", "libpng_build_rule")
load(":libwebp.bzl", "libwebp_build_rule")
load(":google_sparsehash.bzl", "google_sparsehash_build_rule")
load(":libpsl.bzl", "libpsl_build_rule")
load(":giflib.bzl", "giflib_build_rule")
load(":optipng.bzl", "optipng_build_rule")
load(":apr.bzl", "apr_build_rule")
load(":aprutil.bzl", "aprutil_build_rule")
load(":cyclone.bzl", "cyclone_build_rule")
load(":ed25519.bzl", "ed25519_build_rule")
load(":zlib_compat.bzl", "zlib_ng_alias_repository")
# NOTE: do NOT `load(":libavif.bzl"/":libaom.bzl"/":libdav1d.bzl", ...)` here.
# Those files top-level-load @rules_foreign_cc//foreign_cc:defs.bzl, but this
# module is loaded from WORKSPACE *before* the rules_foreign_cc http_archive is
# declared -- importing them creates a "repository used prior to being defined"
# repo-mapping cycle. The *_src archives only need the trivial all_srcs
# filegroup, so they reuse _ALL_SRCS_BUILD_FILE (below), exactly like curl/
# memcached. The cmake() macros in those .bzl files are loaded later, at BUILD
# loading time, which is fine.

# CVE matcher: every vendored C/C++ dep below carries a CPE +
# release_date annotation in tools/dependency/cpe-map.yaml, scanned daily
# against NVD by tools/dependency/cve_scan.py. tools/dependency/validate-deps.py
# fails CI on any dep here that lacks an entry (or an explicit cpe: "N/A" +
# justification) — a new C/C++ dep cannot land unscanned. When you add or bump a
# dep, update its cpe-map.yaml release_date.

ENVOY_COMMIT = "f97695a50e11f5ff6719e129a466bf9204b64a7f"  # v1.37.5 - 2026-06-26 CVE batch (defense-in-depth; mpp's compiled extension set was already unaffected — see tools/dependency/cve-ignore.yaml). ABI-compat pins (BoringSSL/zlib-ng below) unchanged v1.37.2->v1.37.5.
ENVOY_SHA = "b517189c09755bcf24a0e04376f4f329df83aad03ed014857a506c74ce9c103f"

# Standalone zlib-ng — replaces @envoy//bazel:zlib for non-Envoy builds.
ZLIB_NG_VERSION = "2.3.2"
ZLIB_NG_SHA = "6a0561b50b8f5f6434a6a9e667a67026f2b2064a1ffa959c6b2dae320161c2a8"

# Standalone BoringSSL — previously only an Envoy transitive dep.
# Bumped May 2026: ~12 months of upstream drift (post-quantum + hardening rolls).
BORINGSSL_VERSION = "0.20260508.0"
BORINGSSL_SHA = "de3371d3fe085afd34778a4c988fb7840b9c92cb21504e674f33ebefd98edc00"

# Protocol Buffers C++ runtime + codegen. Same v31.1 release commit that
# gRPC 1.78.1's grpc_deps() provided before the gRPC dependency was removed
# along with the experimental central controller.
PROTOBUF_COMMIT = "74211c0dfc2777318ab53c2cd2c317a2ef9012de"  # v31.1
PROTOBUF_SHA = "d0e3a75876a81e1536028bb9cf9181382b198da4cc6fa6aef86879ef629ac807"

# Standalone googletest — previously only an Envoy transitive dep.
GOOGLETEST_VERSION = "1.17.0"
GOOGLETEST_SHA = "65fab701d9829d38cb77c14acdc431d2108bfdbf8979e40eb8ae567edf10b27c"

# Standalone fmt + spdlog — previously only Envoy transitive deps.
# Both are header-only libraries used by base/log_shim.
FMT_VERSION = "12.1.0"
FMT_SHA = "695fd197fa5aff8fc67b5f2bbc110490a875cdf7a41686ac8512fb480fa8ada7"
SPDLOG_VERSION = "1.17.0"
SPDLOG_SHA = "d8862955c6d74e5846b3f580b1605d2428b11d97a410d86e2fb13e857cd3a744"

HIREDIS_COMMIT = "1.3.0"  # Updated Jan 2026 - major version upgrade
HIREDIS_SHA = "25cee4500f359cf5cad3b51ed62059aadfc0939b05150c1f19c7e2829123631c"
JSONCPP_COMMIT = "1.9.6"  # Updated Jan 2026
JSONCPP_SHA = "f93b6dd7ce796b13d02c108bc9f79812245a82e577581c4c9aabe57075c90ea2"
LIBPNG_COMMIT = "1.6.58"  # Updated May 2026 - 4 point releases of parser hardening
LIBPNG_SHA = "a9d4df463d36a6e5f9c29bd6f4967312d17e996c1854f3511f833924eb1993cf"
LIBWEBP_COMMIT = "1.5.0"  # Updated Mar 2026 - CVE-2023-4863 fix (heap buffer overflow)
LIBWEBP_SHA = "668c9aba45565e24c27e17f7aaf7060a399f7f31dba6c97a044e1feacb930f37"
GOOGLE_SPARSEHASH_COMMIT = "6ff8809259d2408cb48ae4fa694e80b15b151af3"
GOOGLE_SPARSEHASH_SHA = "4ae105acb6b53f957b6005fa103a9fd342c39dbc7c87673663e782325b8296b3"
GFLAGS_COMMIT = "de1b8d3daa40b5b07208ec9e82f223d430e2ecc1"  # v2.3.0 - Updated Jan 2026
GFLAGS_SHA = "b563851a60342abc35281fa9c684de7e9604a2bf1982c85035180b9ce3afa774"
# googleurl (gurl) — Chromium's URL parser, used by pagespeed/kernel/http for
# URL parsing (parses attacker-controllable href/src/url()). Originally an
# Envoy dep, but referenced directly by core PageSpeed code. Fully
# encapsulated behind the GoogleUrl wrapper.
# Snapshot e6c272102e (Aug 2025) - re-pinned from the stale undated Nov 2022
# snapshot (dd4080fe) to pull in upstream Chromium URL-parser fixes and record
# a dated snapshot. Source is the canonical github.com/google/gurl mirror
# that Envoy itself now uses (the quiche-envoy-integration GCS bucket only
# mirrors Envoy-pinned commits). This is the newest snapshot for which
# bazel/googleurl_visibility.patch still applies cleanly (the Nov 2025 HEAD
# 94ff147 drifted; see openQuestions).
GOOGLEURL_COMMIT = "e6c272102e0554e02c1bb317edff927ee56c7d0b"
GOOGLEURL_SHA = "9b998fea702bfcfa7d8e763389e56a1e889f718a11ada6b7c4e8c77b43d5a999"
# libpsl — maintained Public Suffix List library. Replaces the dead
# Apache-incubator domain_registry_provider ("drp"); same Mozilla PSL dataset,
# built builtin-only with no IDNA runtime. Release dist tarball (ships a
# pre-generated include/libpsl.h; we still generate suffixes_dafsa.h + config.h).
LIBPSL_VERSION = "0.21.5"
LIBPSL_SHA = "1dcc9ceae8b128f3c0b3f654decd0e1e891afc6ff81098f227ef260449dae208"
GIFLIB_COMMIT = "5.2.2"  # Updated Jan 2026
GIFLIB_SHA = "be7ffbd057cadebe2aa144542fd90c6838c6a083b5e8a9048b8ee3b66b29d5fb"
OPTIPNG_COMMIT = "0.7.8"  # Updated Jan 2026 - security fix for GIF decoder buffer overflow (CVE)
OPTIPNG_SHA = "25a3bd68481f21502ccaa0f4c13f84dcf6b20338e4c4e8c51f2cefbd8513398c"
# Upstream libjpeg-turbo tag — May 2026 bump from Chromium-fork v3.1.0 to upstream v3.1.4.1.
# Native Bazel BUILD file at bazel/libjpeg_turbo.BUILD emulates configure_file() for
# src/jconfig.h, src/jconfigint.h, src/jversion.h via sed-based genrules.
LIBJPEG_TURBO_VERSION = "3.1.4.1"
LIBJPEG_TURBO_SHA = "a7da42b640377c2a9a9665e2c4b0ea60cd5599afb48c2521e6df0c9dc9d15a25"
# APR 1.7.x branch head - Updated Feb 2026
APR_COMMIT = "d7a4f5be56969ebb5d2f9d093e17eb39dd016693"
APR_SHA = "5c56af0a8ad7dee32dc381620496f6ebbaa9bf64a470a4ad4fdb0eed84e87fc7"
# APR-util 1.6.5 (tag `1.6.5`, released 2026-08-10) - SECURITY bump Aug 2026.
# Was efbe77e (1.6.x branch head, 2025-01-01, pre-1.6.4). 1.6.4 is the release
# that carries the fixes for the 2026-08-06 advisory batch; 1.6.5 adds the
# oracle-DBD build fix on top (upstream PR 70170) and is the current stable
# release of the 1.6.x branch:
#   CVE-2026-32327 (9.1) XML stack recursion   - fixed in 1.6.4 (<= 1.6.3 affected)
#   CVE-2026-34191 (9.1) SQL injection, oracle DBD driver - fixed in 1.6.4 (1.6.0-1.6.3)
#   CVE-2025-49506 (7.5) non-constant-time apr_password_validate() - fixed in 1.6.4
#   CVE-2026-34501 (7.5) heap overflow, redis client   - fixed in 1.6.4 (1.6.0-1.6.3)
#   CVE-2026-34502 (7.5) heap overflow, memcached client - fixed in 1.6.4 (1.3.0-1.6.3)
# None of the five defects sit in a source file this build compiles (see the
# srcs list in bazel/aprutil.bzl: no xml/, dbd/, redis/, memcache/, and crypto/
# is limited to apr_md5.c + getuuid.c + uuid.c), so 1.1 was not exposed. The
# bump is taken anyway because it is a clean fast-forward on the same branch:
# of the compiled set only xlate/xlate.c changes (a NULL-argument guard in
# apr_xlate_conv_buffer), plus APU_PATCH_VERSION 4 -> 5 in include/apu_version.h.
# No pre-generated header regeneration needed (apu.h.in / apu_config.h.in /
# apr_ldap.h.in unchanged), no ABI change to the bucket-brigade API 1.1 uses.
APRUTIL_COMMIT = "eb1f0c8ebd4f9633c327f595746933da6349dded"
APRUTIL_SHA = "804b8b276b346a69ca91bc704198de9ff4956a9a60a2f1212f350c230ee3dd03"

# Cyclone Cache - high-performance disk cache with scan-resistant CLFUS algorithm
# Requires C++23 - wrapper provides C ABI for C++20 consumers
# Not pinned during development; pinned to a specific commit at release time.

# Cyclone Cache - pinned at release time. This commit carries the
# fork-safe Cache::stop() fix that stops Apache children
# and the nginx master hanging in Cyclone teardown on graceful recycle/reload,
# the small-object tier API: CacheConfig::small_tier_percent,
# tier-routed sync ops, and Cache::small_tier_active(), plus lease-based
# region pinning: borrowed mmap read views are
# protected against circular-buffer wraps for the lease window — the safety
# prerequisite for the zero-copy serving path (CycloneZeroCopy).  This bump
# makes cache teardown join every background thread
# unconditionally: fixes a thread-leak/use-after-free (the Apache
# graceful-restart symptom where a lingering flush thread kept children
# alive -> MPM scoreboard exhaustion) plus a teardown deadlock and an
# in-flight-operation race surfaced by that fix.  This bump adds
# the epoch-checked renew_lease() and force-wrap deadline (ns_until_forced_wrap)
# that the zero-copy serve copy-out path renews against, plus a Volume-lifetime
# use-after-free fix on the renew path.  This bump makes the
# directory insert/remove election verify the full stored key before electing
# update-in-place: previously a different key colliding on the (stripe, bucket,
# 12-bit tag) triple silently destroyed the victim's directory slot on every
# write (deterministic; the victim reads back as a clean NotFound), and adds
# the tag_collision_evictions stat for the full-bucket eviction fallback.
# This bump fixes Volume::open() so non-creators wait for the
# creator to finish initialization: closes a multi-process cache-open race
# where a second opener could observe a half-initialized volume and corrupt
# it or spuriously fail initialization (the lock is kernel-dropped on
# process death, so crash recovery is automatic); it also brings a Cyclone change,
# which removes the dead WriteAggregator and reclaims ~4 MB of committed RSS
# per cache stripe with no API impact.
# This bump makes wrap gating borrow-scoped:
# the per-stripe read lease defers wraps only while read handles are actually
# outstanding (refcounted, released on handle close; ceiling-forced wraps
# reset leaked state), so cache writes no longer starve at capacity under
# steady reads. On-disk format unchanged (v1); adds the borrows_outstanding
# stats gauge (append-only C-struct extension, rebuilt against the vendored
# header by this bump). renew_lease() semantics for open handles unchanged.
# This bump brings the lock-free concurrent read path: the
# reader-side stripe lock and the directory global lock are gone, replaced
# by per-bucket seqlocks and per-thread-sharded read counters + HitTracker,
# so read throughput scales with cores instead of collapsing under
# contention (a same-machine cache A/B showed a ~5x read-throughput gain and
# turned a memory-pressure regime that used to lose to the file cache into a
# win). It also drops the per-write fsync in multi-process mode (removes the
# journal-commit convoy -> higher fill throughput, lower RSS, a Cyclone change),
# and fixes auto stripe sizing: the stripe count now derives from a 32MB
# granularity so small auto-sized volumes get enough stripes for lease-based
# region pinning to stay local (a 256MB cache goes ~70%->90% hit under mixed
# load); the derived stripe count is persisted in the volume header and
# validated on open. Adds stripe_count/stripe_bytes stats (append-only
# C-struct extension, rebuilt against the vendored header by this bump).
# OPERATIONAL: the on-disk volume format major version is bumped (kept in
# lockstep with the document version), so existing cache volumes auto-reset
# (cold-wipe) on first open after upgrade AND on rollback -- expect a
# one-time origin-fetch spike; prefer an off-peak reship window.
# This bump heap-backs the CRC-validation cache: the 65536-slot
# atomic array made sizeof(Volume) ~520 KB, so any stack-allocated Volume
# overflowed a 1 MB thread stack (the Windows default) and crashed with
# STATUS_STACK_OVERFLOW -- caught by the Windows/IIS port's unit tests.
# sizeof(Volume) drops to ~8.5 KB (one pointer indirection on the read path,
# negligible); a static_assert guard keeps future large inline members out.
# No format or API change. Also brings a Cyclone change: the same fix for
# HitTracker (its 4096-stripe array made sizeof(HitTracker) ~1 MB under
# MSVC -- a stack-allocated tracker overflowed the Windows 1 MB default
# thread stack; now heap-backed, sizeof ~120 bytes, static_assert guard).
# This bump hardens the zero-copy read protocol:
# one change adds the reader-side acquire fences the copy-then-verify epoch
# rechecks needed on weakly-ordered CPUs (no-op on x86) and makes the
# aliased-serving contract explicit (forced-wrap deadline polling is
# normative -- the module's serve paths already comply); one change closes a
# wrap-survivor window where a stale directory entry or chain node could
# hand out a borrow that the ordinary forward fill then overwrote
# undetected (positional guard at both read choke points); one change stops the
# multi-process write lock from force-releasing a live-but-stalled holder
# (liveness proof + takeover-generation guard -- overlapping-write fix).
# This bump (wave-3 hardening batch): one change holds the
# per-stripe write lock across the pwrite, closing a microsecond
# reservation-to-pwrite tear window a concurrent reader could observe;
# one change also routes in-place volume-header updates through the mapping on
# Windows, fixing fd writes to mmap'd ranges being silently swallowed on
# some Windows configurations (relevant to the Windows/IIS port). several changes
# add libFuzzer read-gauntlet + nightly stress harnesses and a Windows
# MSVC/AppVerifier CI lane upstream. On-disk format UNCHANGED: no cache
# reset on upgrade or rollback.
# This bump (cyclone 330035e: PRs several changes + the upgrade-safety stack
# several changes) completes the multi-process silent-corruption
# fix line and makes cross-format upgrades unilaterally safe: one change fixes
# shared-cursor wrap adoption (a stale-high peer could re-wrap and reserve
# an OVERLAPPING range -> CRC-clean corruption); one change adds the lifetime-lock
# reset gate (never reset()/wipe a volume under a live peer -- refuse to
# open instead); one change bumps the on-disk format to v6 (8-aligned header tail
# + 8-byte document-slot padding) and fixes the header read-modify-write
# races on the hit-count and remove-alternate paths; one change encodes
# format+geometry into the cache FILENAME (cyclone.dat ->
# cyclone-6-<geohash>.dat), so binaries that disagree on on-disk layout
# open DIFFERENT files and cross-format upgrade overlap (nginx SIGHUP/USR2,
# apachectl graceful, IIS overlapped recycle) can no longer corrupt a live
# peer's cache, regardless of release order; one change adds opt-in startup GC of
# superseded fingerprint files (CacheConfig::gc_superseded_on_start,
# default OFF; we do not enable it, and a legacy cyclone.dat is never
# auto-deleted).
# OPERATIONAL: v6 is a format bump, but with fingerprinted filenames the
# new binary opens a NEW cache file rather than resetting the old one --
# still a cold cache on upgrade (one-time origin-fetch spike; prefer an
# off-peak window), the previous file is left orphaned on disk (reclaim
# manually or via the opt-in GC), and ROLLBACK is WARM: the old binary
# finds its own file untouched.
#
# Bumped to a Cyclone change: Cache::volume_files() (embedder access to the actual
# fingerprint-named on-disk path) and fingerprint-aware resolution for the
# unsized (size==0) open mode.  mod_pagespeed itself always opens with an
# explicit size and never touches the volume file by name, so this is a
# pin-pair bump: the module and the optimizer must pin the SAME cache-library commit (the 2.0
# nginx integration needs these, and the filename fingerprint, the format
# bump and the superseded-file GC only make cross-format upgrades safe if
# all three ship in ONE release).
#
# This bump (cyclone edecf16): one change fixes the write path when a
# directory bucket fills with current-phase entries -- the amplifier that
# makes the post-upgrade refill window worse -- and counts the resulting
# evictions (bucket_full_evictions); one change brings the cross-process reset
# gate (one change) to Windows and adds reset-gate observability
# (resets_gate_verified / resets_under_degraded_gate) plus tests.  The
# three new counters are appended at the stats-struct tail and surfaced
# via the console backend stats.  On-disk format UNCHANGED.  Still a
# pin-pair bump: the module and the optimizer must pin the SAME commit.
#
# Bumped to cyclone 6286a06 (23 commits).  Still a pin-pair bump: the
# optimizer pins this SAME commit, and the module and the optimizer must pin it.  Three
# things matter here.
#
# 1. LICENCE.  Cyclone is Apache-2.0 from the relicense commit in this range
#    onward; every commit up to and including the previous pin was BUSL-1.1,
#    while this repo's SBOM and NOTICE already described the library as
#    Apache-2.0.  At this pin that is finally true of the commit we build.
#
# 2. BREAKING -- ON-DISK FORMAT MAJOR 7 (volume header and
#    document move in lockstep).  The volume filename carries the format
#    major, so an upgraded binary opens a NEW file rather than resetting the
#    old one: every deployment starts with an EMPTY CACHE that refills as
#    traffic arrives.  Rollback stays warm (a format-6 binary reopens its own
#    untouched file).  The superseded file is left on disk -- the opt-in
#    startup GC of superseded volumes is still default-off and
#    not enabled here, so reclaim it manually.
#    START ORDER, and it is load-bearing for the shared-volume deployments:
#    the optimizer daemon must be upgraded and started BEFORE the module
#    process restarts.  This module does not parse the volume format -- the
#    serve path is handed pointer+length by the cache reader, and on the
#    shared-volume path every field arrives through a dynamically loaded
#    daemon accessor -- so format 7 flows through it transparently.  But the
#    module's attach check refuses to start when it sees more than one volume
#    file beside the configured stem, and a module that restarts while the
#    old format-6 file is still the only one on disk and the format-7 daemon
#    has not yet created its own will see exactly that second file appear.
#
# 3. THE DEFECT a Cyclone change FIXES, which has already bitten deployed caches.
#    Re-recording the same alternate id left every superseded document linked
#    in the chain forever: physical chain depth grew one per re-record while
#    the unique-id count stayed put, until the traversal cap refused every
#    further write to that key.  The key then froze -- reads kept serving the
#    last-landed version with no error, so it was served stale and never
#    re-optimised, with no self-healing path.  Post-wrap a dominant key's
#    chain could additionally turn cyclic and wedge earlier.  The write path
#    now unlinks the superseded same-id node during the chain walk it already
#    performs (no extra I/O), refuses to link a new head to nodes from a
#    previous wrap epoch, and carries a single-id conditional chain reset that
#    lets an already-wedged chain accept writes again.  Splicing is
#    best-effort: a splice that cannot proceed safely is deferred and counted,
#    never failing the caller's write.  Default ON; the upstream kill switch
#    is not set here.
#
# Also in the delta, reaching this repo with no config change on our side:
# Cyclone changes make CacheConfig::max_object_size live (declared but never
# read until now), so the 64 MB default is ENFORCED and an over-size put fails
# with the tail-appended CacheError::ObjectTooLarge.  Cyclone changes make
# both built-in alternate selectors treat a non-empty acceptable_alternates
# set as a HARD restriction rather than a hint.  Cyclone changes stop
# the RAM tier from serving an entry that a re-record or a removal should have
# invalidated.  A Cyclone change zeroes a wrap deadline left uninitialised in
# MmapDirectory::init.  A Cyclone change adds opt-in cross-process RAM-cache
# coherence: default OFF and not plumbed through this repo's cache config, so
# it is inert here.  A Cyclone change plus one change's alternate-chain counters
# tail-append to the stats structs, transparent to this repo's field-by-field
# stats consumers; surfacing them is a follow-up.  CacheConfig::min_object_size
# was removed upstream; this repo never set it.  The vendored
# build glue in //bazel:cyclone.bzl needs no change: the delta adds test
# sources only, and every glob in cyclone_build_rule matches the same file set
# at both pins.
#
# A Cyclone change fixes a use-after-free on the write path: a Cyclone change made the
# per-object bound read Volume::config() at the top of every write, which
# turned the write handle's long-standing raw Volume* into a live
# use-after-free the moment a handle outlived its volume -- a cache reset or a
# teardown under an open handle.  The handle now holds a weak reference, pins
# the volume for the duration of write() and the commit, and reports
# CacheError::Closed when the volume is gone.  Module and optimizer pin the same commit; the pin-pair gate checks that.
CYCLONE_COMMIT = "962f2e8458db72ef6aaa808b317253baac04ce78"

# Libevent - cross-platform event notification library
# Used by LibeventDispatcher for standalone event loop (Apache deployments)
LIBEVENT_VERSION = "2.1.12-stable"
LIBEVENT_SHA = "92e6de1be9ec176428fd2367677e61ceffc2ee1cb119035037a27d346b0403bb"

# libcurl - HTTP client library (built from source)
LIBCURL_VERSION = "8.21.0"
LIBCURL_SHA = "ec753aa6f408a3ca9f0d6d5f7a77417aecd1544db13c03ae5d443612bf367364"

# libmemcached - memcached client library (built from source)
# Using awesomized/libmemcached fork which is actively maintained
LIBMEMCACHED_VERSION = "1.1.4"
LIBMEMCACHED_SHA = "c477e1f6510e1dc698e84f3717ce690a8f65b94c616ecaa62306cce0f5e3116a"

# AVIF stack. Built from source via rules_foreign_cc:
#   libavif  -> cmake  (bazel/libavif.bzl)
#   libaom   -> cmake  (bazel/libaom.bzl)   AV1 encoder+decoder (reference)
#   dav1d    -> meson  (bazel/libdav1d.bzl) faster AV1 decoder (optional)
# Source-stability decisions (Stream 0 spike): libavif ships a byte-stable
# GitHub release tarball; dav1d uses the byte-stable GitHub MIRROR release tag
# (videolan/dav1d), NOT the code.videolan.org GitLab auto-tarball; aom uses a
# git_repository commit pin (aomedia.googlesource.com is git-only and its
# archive tarballs are not byte-stable), mirroring the cyclone pattern.
# NOTE: each dep also needs a tools/dependency/cpe-map.yaml entry — the
# completeness gate fails CI on a dep that has none.
LIBAVIF_VERSION = "1.4.2"
LIBAVIF_SHA = "2b645287340ba5a631d268b551dc2d72bd73ac33335962dd36dcdb6d8366921d"
DAV1D_VERSION = "1.5.1"
DAV1D_SHA = "fa635e2bdb25147b1384007c83e15de44c589582bb3b9a53fc1579cb9d74b695"
AOM_COMMIT = "3b624af45b86646a20b11a9ff803aeae588cdee6"  # v3.12.0 (dereferenced tag)

# Build file content for source archives used by rules_foreign_cc
_ALL_SRCS_BUILD_FILE = """
filegroup(
    name = "all_srcs",
    srcs = glob(["**"]),
    visibility = ["//visibility:public"],
)
"""

# libavif_src additionally exposes its SOURCE include tree as a header-only
# cc_library: the clang-tidy compdb build in CI's lint job
# (--output_groups=compdb_files,header_files) never runs the cmake() install,
# so TUs including "avif/avif.h" can only resolve it against the source
# headers (byte-identical to the cmake-installed copies; the duplicate -I in
# real builds is harmless). load() must lead the file, so this is a full
# standalone string rather than _ALL_SRCS_BUILD_FILE + suffix.
_LIBAVIF_SRCS_BUILD_FILE = """
load("@rules_cc//cc:defs.bzl", "cc_library")

filegroup(
    name = "all_srcs",
    srcs = glob(["**"]),
    visibility = ["//visibility:public"],
)

cc_library(
    name = "source_headers",
    hdrs = glob(["include/avif/*.h"]),
    includes = ["include"],
    visibility = ["//visibility:public"],
)
"""

_ABSEIL_VERSION = "20260107.1"
_ABSEIL_SHA = "4314e2a7cbac89cac25a2f2322870f343d81579756ceff7f431803c2c9090195"
_ABSEIL_PATCHES = [
    "//bazel:abseil.patch",
    "//bazel:abseil_nullability.patch",
]

def mod_pagespeed_dependencies():
    # Declare abseil under both names used in the dependency graph:
    # - "com_google_absl": PageSpeed BUILD files
    # - "abseil-cpp": protobuf transitive deps
    # Both must exist with identical content to satisfy Bazel's strict
    # include checking on Windows (headers from either external/ dir
    # must be matched by a declared dep).
    for abseil_name in ["com_google_absl", "abseil-cpp"]:
        http_archive(
            name = abseil_name,
            urls = ["https://github.com/abseil/abseil-cpp/archive/%s.tar.gz" % _ABSEIL_VERSION],
            sha256 = _ABSEIL_SHA,
            strip_prefix = "abseil-cpp-%s" % _ABSEIL_VERSION,
            patches = _ABSEIL_PATCHES,
            patch_args = ["-p1"],
        )

    # Phase 0: Standalone zlib-ng
    http_archive(
        name = "zlib_ng",
        strip_prefix = "zlib-ng-%s" % ZLIB_NG_VERSION,
        url = "https://github.com/zlib-ng/zlib-ng/archive/%s.tar.gz" % ZLIB_NG_VERSION,
        sha256 = ZLIB_NG_SHA,
        build_file = "//bazel:zlib_ng.BUILD",
    )

    # Collapse the duplicate deflate: alias @zlib -> @zlib_ng so protobuf's
    # gzip_stream links the SAME single zlib-ng as libpng/kernel/util,
    # instead of dragging in a second (stock madler) zlib. Declared here, before
    # protobuf_deps() in WORKSPACE, so its maybe()-guarded madler
    # @zlib is skipped. Fixes the ODR/UB two-deflate hazard (was the PngOptimizer
    # golden "flake"). See bazel/zlib_compat.bzl.
    zlib_ng_alias_repository(name = "zlib")

    # Phase 1: Standalone BoringSSL
    http_archive(
        name = "boringssl",
        strip_prefix = "boringssl-%s" % BORINGSSL_VERSION,
        url = "https://github.com/google/boringssl/archive/%s.tar.gz" % BORINGSSL_VERSION,
        sha256 = BORINGSSL_SHA,
        # Add CRYPTO_thread_local_cleanup() so pagespeed_iis.dll can release
        # the BoringSSL TLS slot on DLL unload (issue one change).
        patches = ["@mod_pagespeed//bazel:boringssl_dll_unload_tls_cleanup.patch"],
        patch_args = ["-p1"],
    )

    # Protocol Buffers. Its @abseil-cpp refs resolve directly against the
    # abseil archive declared above (both abseil names exist).
    http_archive(
        name = "com_google_protobuf",
        strip_prefix = "protobuf-%s" % PROTOBUF_COMMIT,
        url = "https://github.com/protocolbuffers/protobuf/archive/%s.tar.gz" % PROTOBUF_COMMIT,
        sha256 = PROTOBUF_SHA,
        # Redirect protobuf's internal @abseil-cpp refs to the canonical
        # @com_google_absl repo (same wiring gRPC's grpc_deps() declaration
        # used). Without this, protobuf's absl deps resolve to the duplicate
        # "abseil-cpp" archive, and Windows strict-include validation flags
        # every absl header a protobuf header pulls in (e.g. absl/log/*) as an
        # undeclared inclusion in dependents.
        repo_mapping = {"@abseil-cpp": "@com_google_absl"},
    )

    # Phase 2: Standalone libevent.
    # maybe(): WORKSPACE.envoy pre-defines this repo with Envoy's patched
    # snapshot (event2/watch.h) before calling mod_pagespeed_dependencies();
    # the lean WORKSPACE does not, so it gets this vanilla release.
    maybe(
        http_archive,
        name = "com_github_libevent_libevent",
        strip_prefix = "libevent-%s" % LIBEVENT_VERSION,
        url = "https://github.com/libevent/libevent/releases/download/release-%s/libevent-%s.tar.gz" % (LIBEVENT_VERSION, LIBEVENT_VERSION),
        sha256 = LIBEVENT_SHA,
        build_file_content = """
filegroup(
    name = "all_srcs",
    srcs = glob(["**"]),
    visibility = ["//visibility:public"],
)
filegroup(
    name = "all",
    srcs = glob(["**"]),
    visibility = ["//visibility:public"],
)
""",
    )

    # Standalone googletest — previously only an Envoy transitive dep.
    http_archive(
        name = "googletest",
        strip_prefix = "googletest-%s" % GOOGLETEST_VERSION,
        url = "https://github.com/google/googletest/releases/download/v%s/googletest-%s.tar.gz" % (GOOGLETEST_VERSION, GOOGLETEST_VERSION),
        sha256 = GOOGLETEST_SHA,
        # Redirect googletest's @abseil-cpp refs (activated by --define absl=1
        # in .bazelrc) to our canonical repo. Without this, googletest adds
        # external/abseil-cpp/ to include paths on Windows.
        repo_mapping = {"@abseil-cpp": "@com_google_absl"},
    )

    # re2 is used directly by pagespeed (pagespeed/kernel/util/re2.h, via
    # @com_googlesource_code_re2) and by googletest (which references @re2
    # when built with absl). Both names must resolve to the same version.
    # Bumped May 2026 from 2022-04-01 (~4 years of upstream drift); parity
    # with MPS 2.0 which already runs this version. The Windows strict-deps
    # fallout from this bump is fixed independently in test/pagespeed/kernel/util/BUILD.
    _RE2_VERSION = "2025-11-05"
    _RE2_SHA = "87f6029d2f6de8aa023654240a03ada90e876ce9a4676e258dd01ea4c26ffd67"
    for re2_name in ["com_googlesource_code_re2", "re2"]:
        http_archive(
            name = re2_name,
            strip_prefix = "re2-%s" % _RE2_VERSION,
            urls = ["https://github.com/google/re2/archive/%s.tar.gz" % _RE2_VERSION],
            sha256 = _RE2_SHA,
        )

    # Standalone fmt — header-only formatting library, dep of spdlog.
    http_archive(
        name = "fmt",
        strip_prefix = "fmt-%s" % FMT_VERSION,
        url = "https://github.com/fmtlib/fmt/releases/download/%s/fmt-%s.zip" % (FMT_VERSION, FMT_VERSION),
        sha256 = FMT_SHA,
        build_file_content = """
cc_library(
    name = "fmt",
    hdrs = glob(["include/fmt/*.h"]),
    includes = ["include"],
    defines = ["FMT_HEADER_ONLY"],
    visibility = ["//visibility:public"],
)
""",
    )

    # Standalone spdlog — header-only logging library, used by base/log_shim.
    http_archive(
        name = "spdlog",
        strip_prefix = "spdlog-%s" % SPDLOG_VERSION,
        url = "https://github.com/gabime/spdlog/archive/v%s.tar.gz" % SPDLOG_VERSION,
        sha256 = SPDLOG_SHA,
        build_file_content = """
cc_library(
    name = "spdlog",
    hdrs = glob(["include/**/*.h"]),
    includes = ["include"],
    defines = ["SPDLOG_FMT_EXTERNAL", "SPDLOG_NO_EXCEPTIONS", "FMT_HEADER_ONLY"],
    deps = ["@fmt"],
    visibility = ["//visibility:public"],
)
""",
    )

    http_archive(
        name = "envoy",
        strip_prefix = "envoy-%s" % ENVOY_COMMIT,
        url = "https://github.com/envoyproxy/envoy/archive/%s.tar.gz" % ENVOY_COMMIT,
        sha256 = ENVOY_SHA,
        patches = ["//bazel:envoy_repo_yq_windows.patch"],
        patch_args = ["-p1"],
    )

    http_archive(
        name = "hiredis",
        strip_prefix = "hiredis-%s" % HIREDIS_COMMIT,
        url = "https://github.com/redis/hiredis/archive/v%s.tar.gz" % HIREDIS_COMMIT,
        build_file_content = hiredis_build_rule,
        sha256 = HIREDIS_SHA,
    )

    http_archive(
        name = "jsoncpp",
        strip_prefix = "jsoncpp-%s" % JSONCPP_COMMIT,
        url = "https://github.com/open-source-parsers/jsoncpp/archive/%s.tar.gz" % JSONCPP_COMMIT,
        build_file_content = jsoncpp_build_rule,
        sha256 = JSONCPP_SHA,
    )

    http_archive(
        name = "libpng",
        strip_prefix = "libpng-%s" % LIBPNG_COMMIT,
        url = "https://github.com/glennrp/libpng/archive/v%s.tar.gz" % LIBPNG_COMMIT,
        build_file_content = libpng_build_rule,
        sha256 = LIBPNG_SHA,
    )

    http_archive(
        name = "libwebp",
        urls = [
            "https://github.com/webmproject/libwebp/archive/v%s.tar.gz" % LIBWEBP_COMMIT,
        ],
        sha256 = LIBWEBP_SHA,
        strip_prefix = "libwebp-%s" % LIBWEBP_COMMIT,
        build_file_content = libwebp_build_rule,
    )

    http_archive(
        name = "google_sparsehash",
        strip_prefix = "sparsehash-%s" % GOOGLE_SPARSEHASH_COMMIT,
        url = "https://github.com/sparsehash/sparsehash/archive/%s.tar.gz" % GOOGLE_SPARSEHASH_COMMIT,
        build_file_content = google_sparsehash_build_rule,
        sha256 = GOOGLE_SPARSEHASH_SHA,
        patches = ["@mod_pagespeed//bazel:sparsehash_cstring.patch"],
        patch_args = ["-p1"],
    )

    http_archive(
        name = "com_googlesource_googleurl",
        sha256 = GOOGLEURL_SHA,
        strip_prefix = "gurl-%s" % GOOGLEURL_COMMIT,
        urls = ["https://github.com/google/gurl/archive/%s.tar.gz" % GOOGLEURL_COMMIT],
        patches = ["@mod_pagespeed//bazel:googleurl_visibility.patch"],
        patch_args = ["-p1"],
        # NOTE: the former Darwin-only patch_cmds sed renaming
        # __is_cpp17_contiguous_iterator -> __libcpp_is_contiguous_iterator
        # was removed: current libc++ (Xcode 16+ / macOS 26) already uses the
        # new spelling, and the pinned gurl snapshot now carries an upstream
        # block with that spelling too, so the sed produced a
        # duplicate-definition error.
    )

    http_archive(
        name = "com_github_gflags_gflags",
        strip_prefix = "gflags-%s" % GFLAGS_COMMIT,
        url = "https://github.com/gflags/gflags/archive/%s.tar.gz" % GFLAGS_COMMIT,
        sha256 = GFLAGS_SHA,
    )

    http_archive(
        name = "libpsl",
        url = "https://github.com/rockdaboot/libpsl/releases/download/%s/libpsl-%s.tar.gz" % (LIBPSL_VERSION, LIBPSL_VERSION),
        build_file_content = libpsl_build_rule,
        strip_prefix = "libpsl-%s" % LIBPSL_VERSION,
        sha256 = LIBPSL_SHA,
    )

    http_archive(
        name = "giflib",
        strip_prefix = "giflib-%s" % GIFLIB_COMMIT,
        url = "https://sourceforge.net/projects/giflib/files/giflib-5.x/giflib-%s.tar.gz/download" % GIFLIB_COMMIT,
        type = "tar.gz",
        build_file_content = giflib_build_rule,
        sha256 = GIFLIB_SHA,
    )

    http_archive(
        name = "optipng",
        strip_prefix = "optipng-%s" % OPTIPNG_COMMIT,
        url = "https://prdownloads.sourceforge.net/optipng/optipng-%s.tar.gz?download" % OPTIPNG_COMMIT,
        build_file_content = optipng_build_rule,
        sha256 = OPTIPNG_SHA,
    )

    http_archive(
        name = "libjpeg_turbo",
        strip_prefix = "libjpeg-turbo-%s" % LIBJPEG_TURBO_VERSION,
        url = "https://github.com/libjpeg-turbo/libjpeg-turbo/archive/refs/tags/%s.tar.gz" % LIBJPEG_TURBO_VERSION,
        build_file = "//bazel:libjpeg_turbo.BUILD",
        sha256 = LIBJPEG_TURBO_SHA,
    )

    http_archive(
        name = "apr",
        strip_prefix = "apr-%s" % APR_COMMIT,
        url = "https://github.com/apache/apr/archive/%s.tar.gz" % APR_COMMIT,
        build_file_content = apr_build_rule,
        patches = ["apr.patch"],
        patch_args = ["-p1"],
        sha256 = APR_SHA,
    )

    http_archive(
        name = "aprutil",
        strip_prefix = "apr-util-%s" % APRUTIL_COMMIT,
        url = "https://github.com/apache/apr-util/archive/%s.tar.gz" % APRUTIL_COMMIT,
        build_file_content = aprutil_build_rule,
        sha256 = APRUTIL_SHA,
    )

    # Cyclone Cache - high-performance disk cache
    git_repository(
        name = "cyclone",
        remote = "https://github.com/We-Amp/cyclone-cache.git",
        commit = CYCLONE_COMMIT,
        build_file_content = cyclone_build_rule,
    )

    # libcurl source - built via cmake in //bazel:curl
    http_archive(
        name = "curl_src",
        strip_prefix = "curl-curl-%s" % LIBCURL_VERSION.replace(".", "_"),
        url = "https://github.com/curl/curl/archive/refs/tags/curl-%s.tar.gz" % LIBCURL_VERSION.replace(".", "_"),
        sha256 = LIBCURL_SHA,
        build_file_content = _ALL_SRCS_BUILD_FILE,
        patches = ["@mod_pagespeed//bazel:curl_boringssl_ssl_connect_8_20.patch"],
        patch_args = ["-p1"],
    )

    # libmemcached source - built via cmake in //bazel:libmemcached
    http_archive(
        name = "libmemcached_src",
        strip_prefix = "libmemcached-%s" % LIBMEMCACHED_VERSION,
        url = "https://github.com/awesomized/libmemcached/archive/refs/tags/%s.tar.gz" % LIBMEMCACHED_VERSION,
        sha256 = LIBMEMCACHED_SHA,
        build_file_content = _ALL_SRCS_BUILD_FILE,
    )

    # ---- AVIF stack ------------------------------------------------------
    # libavif source - built via cmake in //bazel:avif (codecs threaded as
    # cmake deps; see bazel/libavif.bzl).
    http_archive(
        name = "libavif_src",
        strip_prefix = "libavif-%s" % LIBAVIF_VERSION,
        url = "https://github.com/AOMediaCodec/libavif/archive/refs/tags/v%s.tar.gz" % LIBAVIF_VERSION,
        sha256 = LIBAVIF_SHA,
        build_file_content = _LIBAVIF_SRCS_BUILD_FILE,
        # Static libavif installs a MERGED archive, and its merge helper picks a
        # bundling tool by compiler ID -- which routes clang-cl into the GNU `ar`
        # branch and produces no archive at all. See the patch header for the
        # full diagnosis. No effect on the Linux/macOS legs.
        patches = ["//bazel:libavif_merge_static_libs_clang_cl.patch"],
        patch_args = ["-p1"],
    )

    # dav1d source - built via meson in //bazel:dav1d. GitHub mirror release
    # tag (byte-stable), NOT the GitLab auto-tarball.
    http_archive(
        name = "dav1d_src",
        strip_prefix = "dav1d-%s" % DAV1D_VERSION,
        url = "https://github.com/videolan/dav1d/archive/refs/tags/%s.tar.gz" % DAV1D_VERSION,
        sha256 = DAV1D_SHA,
        build_file_content = _ALL_SRCS_BUILD_FILE,
    )

    # aom source - built via cmake in //bazel:aom. git_repository commit pin
    # (googlesource is git-only; archive tarballs are not byte-stable).
    git_repository(
        name = "aom_src",
        remote = "https://aomedia.googlesource.com/aom",
        commit = AOM_COMMIT,
        build_file_content = _ALL_SRCS_BUILD_FILE,
    )

    # orlp/ed25519 - compact Ed25519 implementation
    http_archive(
        name = "ed25519",
        strip_prefix = "ed25519-b1f19fab4aebe607805620d25a5e42566ce46a0e",
        url = "https://github.com/orlp/ed25519/archive/b1f19fab4aebe607805620d25a5e42566ce46a0e.tar.gz",
        build_file_content = ed25519_build_rule,
        sha256 = "aedb26c46d3dc3b721ab37c5248d5c923142e4d56009a9605c470383f32ce77a",
    )

