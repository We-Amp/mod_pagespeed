// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Compile-time pins for the published lib/pagespeed struct layouts.
//
// A consumer of this library is compiled somewhere else, at some other time,
// against some other copy of pagespeed.h. Everything it knows about a struct
// is the offsets its own compiler baked in. Move a field and that consumer
// keeps reading the same bytes with a different meaning -- no crash, no
// diagnostic, just wrong values, on their machine and not ours.
//
// So the layout is asserted, not assumed, and asserted at COMPILE time: these
// fire on every platform and architecture CI builds for, which is exactly
// where an alignment difference would show up and a runtime check on one
// machine would not.
//
// If one of these fires, the question is not "what is the new number". It is
// which rule in lib/pagespeed/ABI.md the change broke, and whether the field
// should have been APPENDED instead. Fields are appended, never inserted.
//
// tools/ci/check_abi.py reads BOTH the sizeof and the offsetof assertions
// below and cross-checks them against lib/pagespeed/abi/abi-golden.json, so
// the two records of the ABI cannot drift apart without something going red.
// Sizes alone would not be enough: swapping two same-width fields leaves the
// size untouched, so the gate rejects any struct pinned by size with no offset
// assertions at all.

#include <stddef.h>

#include "gtest/gtest.h"
#include "lib/pagespeed/pagespeed.h"

namespace {

// ---------------------------------------------------------------------------
// ps_notify_params_t -- added whole at 1.5; `site` and `notify_flags`
// appended at 1.12.
//
// The 4-byte hole after agent_request (44..47) is alignment padding ahead of
// the pointer at 48, not a reserved field: it is the compiler's, it is
// identical on every LP64 target, and ps_notify_params_init_sized zeroes the
// whole struct, so nothing is ever read out of it. Since 1.12 the struct ends
// on the 4-byte notify_flags at 80, followed by 4 bytes of tail padding up to
// the pointer alignment; the library reads the site fields only when
// struct_size is at least 88, so the padding is never read either.
// ---------------------------------------------------------------------------
static_assert(sizeof(ps_notify_params_t) == 88);
static_assert(offsetof(ps_notify_params_t, struct_size) == 0);
static_assert(offsetof(ps_notify_params_t, url) == 8);
static_assert(offsetof(ps_notify_params_t, hostname) == 16);
static_assert(offsetof(ps_notify_params_t, scheme) == 24);
static_assert(offsetof(ps_notify_params_t, content_type) == 32);
static_assert(offsetof(ps_notify_params_t, mask) == 36);
static_assert(offsetof(ps_notify_params_t, agent_request) == 40);
static_assert(offsetof(ps_notify_params_t, option_context) == 48);
static_assert(offsetof(ps_notify_params_t, option_context_length) == 56);
static_assert(offsetof(ps_notify_params_t, option_signature) == 64);
static_assert(offsetof(ps_notify_params_t, site) == 72);
static_assert(offsetof(ps_notify_params_t, notify_flags) == 80);

// ---------------------------------------------------------------------------
// ps_write_params_t -- 1.1 prefix through origin_ct, origin state appended
// at 1.2, origin_html_hash appended at 1.13 after the 1.2 _reserved tail.
// ---------------------------------------------------------------------------
static_assert(sizeof(ps_write_params_t) == 88);
static_assert(offsetof(ps_write_params_t, struct_size) == 0);
static_assert(offsetof(ps_write_params_t, alternate_id) == 8);
static_assert(offsetof(ps_write_params_t, content_length) == 16);
static_assert(offsetof(ps_write_params_t, full_mask) == 24);
static_assert(offsetof(ps_write_params_t, content_type) == 28);
static_assert(offsetof(ps_write_params_t, flags) == 32);
static_assert(offsetof(ps_write_params_t, origin_ct) == 40);
// --- appended at 1.2; nothing above may move ---
static_assert(offsetof(ps_write_params_t, origin_etag) == 48);
static_assert(offsetof(ps_write_params_t, cache_inserted_at) == 56);
static_assert(offsetof(ps_write_params_t, origin_max_age) == 60);
static_assert(offsetof(ps_write_params_t, origin_s_maxage) == 64);
static_assert(offsetof(ps_write_params_t, origin_last_modified) == 68);
static_assert(offsetof(ps_write_params_t, origin_cc_flags) == 72);
// --- appended at 1.13; nothing above may move ---
static_assert(offsetof(ps_write_params_t, origin_html_hash) == 80);

// ---------------------------------------------------------------------------
// ps_sidecar_write_params_t -- added whole at 1.13.
// ---------------------------------------------------------------------------
static_assert(sizeof(ps_sidecar_write_params_t) == 48);
static_assert(offsetof(ps_sidecar_write_params_t, struct_size) == 0);
static_assert(offsetof(ps_sidecar_write_params_t, url) == 8);
static_assert(offsetof(ps_sidecar_write_params_t, hostname) == 16);
static_assert(offsetof(ps_sidecar_write_params_t, scheme) == 24);
static_assert(offsetof(ps_sidecar_write_params_t, site) == 32);
static_assert(offsetof(ps_sidecar_write_params_t, origin_html_hash) == 40);

// ---------------------------------------------------------------------------
// ps_cache_config_t -- mirrored field-by-field by the .NET binding.
// ---------------------------------------------------------------------------
static_assert(sizeof(ps_cache_config_t) == 48);
static_assert(offsetof(ps_cache_config_t, struct_size) == 0);
static_assert(offsetof(ps_cache_config_t, volume_path) == 8);
static_assert(offsetof(ps_cache_config_t, volume_size) == 16);
static_assert(offsetof(ps_cache_config_t, enable_checksum) == 24);
static_assert(offsetof(ps_cache_config_t, ram_cache_size) == 32);
static_assert(offsetof(ps_cache_config_t, max_metadata_size) == 40);

// ---------------------------------------------------------------------------
// ps_alternate_info_t / ps_cache_stats_t
// ---------------------------------------------------------------------------
static_assert(sizeof(ps_alternate_info_t) == 32);
static_assert(offsetof(ps_alternate_info_t, struct_size) == 0);
static_assert(offsetof(ps_alternate_info_t, content_length) == 8);
static_assert(offsetof(ps_alternate_info_t, hit_count) == 16);
static_assert(offsetof(ps_alternate_info_t, alternate_id) == 24);

static_assert(sizeof(ps_cache_stats_t) == 112);
static_assert(offsetof(ps_cache_stats_t, struct_size) == 0);
static_assert(offsetof(ps_cache_stats_t, ram_cache_hits) == 8);
static_assert(offsetof(ps_cache_stats_t, ram_cache_misses) == 16);
static_assert(offsetof(ps_cache_stats_t, disk_cache_hits) == 24);
static_assert(offsetof(ps_cache_stats_t, disk_cache_misses) == 32);
static_assert(offsetof(ps_cache_stats_t, bytes_read) == 40);
static_assert(offsetof(ps_cache_stats_t, bytes_written) == 48);
static_assert(offsetof(ps_cache_stats_t, evictions) == 56);
static_assert(offsetof(ps_cache_stats_t, current_entries) == 64);
static_assert(offsetof(ps_cache_stats_t, current_size_bytes) == 72);
static_assert(offsetof(ps_cache_stats_t, volume_capacity_bytes) == 80);
static_assert(offsetof(ps_cache_stats_t, ram_cache_bytes) == 88);
static_assert(offsetof(ps_cache_stats_t, total_hits) == 96);
static_assert(offsetof(ps_cache_stats_t, total_misses) == 104);

// ---------------------------------------------------------------------------
// ps_serve_stats_snapshot_t
// ---------------------------------------------------------------------------
static_assert(sizeof(ps_serve_stats_snapshot_t) == 96);
static_assert(offsetof(ps_serve_stats_snapshot_t, struct_size) == 0);
static_assert(offsetof(ps_serve_stats_snapshot_t, serve_optimized_total) == 8);
static_assert(offsetof(ps_serve_stats_snapshot_t, serve_original_cold_total) ==
              16);
static_assert(offsetof(ps_serve_stats_snapshot_t,
                       serve_original_pending_total) == 24);
static_assert(offsetof(ps_serve_stats_snapshot_t,
                       serve_original_declined_total) == 32);
static_assert(offsetof(ps_serve_stats_snapshot_t, serve_original_skew_total) ==
              40);
static_assert(offsetof(ps_serve_stats_snapshot_t, notify_suppressed_total) ==
              48);
static_assert(offsetof(ps_serve_stats_snapshot_t,
                       serve_class_unrecognized_total) == 56);
static_assert(offsetof(ps_serve_stats_snapshot_t,
                       serve_flags_unrecognized_total) == 64);
static_assert(offsetof(ps_serve_stats_snapshot_t, saturation_sample_accum) ==
              72);
static_assert(offsetof(ps_serve_stats_snapshot_t, saturation_sample_count) ==
              80);
static_assert(offsetof(ps_serve_stats_snapshot_t, worker_pool_threads) == 88);
static_assert(offsetof(ps_serve_stats_snapshot_t, saturation_hwm) == 92);

// ---------------------------------------------------------------------------
// HTML configuration structs. ps_html_config_t is the one that has already
// grown (enable_async_css, then scheme, appended), which is why its size is
// pinned.
// ---------------------------------------------------------------------------
static_assert(sizeof(ps_html_config_t) == 136);
static_assert(offsetof(ps_html_config_t, struct_size) == 0);
static_assert(offsetof(ps_html_config_t, enable_critical_css) == 8);
static_assert(offsetof(ps_html_config_t, viewport) == 44);
static_assert(offsetof(ps_html_config_t, max_html_size) == 48);
static_assert(offsetof(ps_html_config_t, max_css_size) == 56);
static_assert(offsetof(ps_html_config_t, always_include_selectors) == 64);
static_assert(offsetof(ps_html_config_t, exclude_tag_patterns) == 112);
static_assert(offsetof(ps_html_config_t, enable_async_css) == 120);
static_assert(offsetof(ps_html_config_t, scheme) == 128);

static_assert(sizeof(ps_critical_css_config_t) == 88);
static_assert(offsetof(ps_critical_css_config_t, struct_size) == 0);
static_assert(offsetof(ps_critical_css_config_t, max_elements) == 8);
static_assert(offsetof(ps_critical_css_config_t, max_depth) == 12);
static_assert(offsetof(ps_critical_css_config_t, viewport) == 16);
static_assert(offsetof(ps_critical_css_config_t, max_css_size) == 24);
static_assert(offsetof(ps_critical_css_config_t, always_include_selectors) ==
              32);
static_assert(offsetof(ps_critical_css_config_t, exclude_tag_patterns) == 80);

// ---------------------------------------------------------------------------
// HTTP cache policy structs (new at 1.2).
// ---------------------------------------------------------------------------
static_assert(sizeof(ps_freshness_config_t) == 32);
static_assert(offsetof(ps_freshness_config_t, struct_size) == 0);
static_assert(offsetof(ps_freshness_config_t, max_age_cap) == 8);
static_assert(offsetof(ps_freshness_config_t, immutable_max_age_cap) == 12);
static_assert(offsetof(ps_freshness_config_t, html_max_age) == 16);
static_assert(offsetof(ps_freshness_config_t, css_max_age) == 20);
static_assert(offsetof(ps_freshness_config_t, image_max_age) == 24);

static_assert(sizeof(ps_freshness_input_t) == 40);
static_assert(offsetof(ps_freshness_input_t, struct_size) == 0);
static_assert(offsetof(ps_freshness_input_t, now_seconds) == 8);
static_assert(offsetof(ps_freshness_input_t, cache_inserted_at) == 12);
static_assert(offsetof(ps_freshness_input_t, origin_max_age) == 16);
static_assert(offsetof(ps_freshness_input_t, origin_s_maxage) == 20);
static_assert(offsetof(ps_freshness_input_t, content_type) == 24);
static_assert(offsetof(ps_freshness_input_t, cache_scope) == 28);
static_assert(offsetof(ps_freshness_input_t, force_revalidate) == 32);
static_assert(offsetof(ps_freshness_input_t, origin_cc_flags) == 36);

static_assert(sizeof(ps_freshness_result_t) == 32);
static_assert(offsetof(ps_freshness_result_t, struct_size) == 0);
static_assert(offsetof(ps_freshness_result_t, verdict) == 8);
static_assert(offsetof(ps_freshness_result_t, age_seconds) == 12);
static_assert(offsetof(ps_freshness_result_t, effective_max_age) == 16);
static_assert(offsetof(ps_freshness_result_t, remaining_ttl) == 20);
static_assert(offsetof(ps_freshness_result_t, is_stale) == 24);
static_assert(offsetof(ps_freshness_result_t, expired_by_age) == 28);

static_assert(sizeof(ps_cache_control_input_t) == 40);
static_assert(offsetof(ps_cache_control_input_t, struct_size) == 0);
static_assert(offsetof(ps_cache_control_input_t, mode) == 8);
static_assert(offsetof(ps_cache_control_input_t, cache_scope) == 12);
static_assert(offsetof(ps_cache_control_input_t, effective_max_age) == 16);
static_assert(offsetof(ps_cache_control_input_t, origin_max_age) == 20);
static_assert(offsetof(ps_cache_control_input_t, content_type) == 24);
static_assert(offsetof(ps_cache_control_input_t, origin_cc_flags) == 28);
static_assert(offsetof(ps_cache_control_input_t, synthesize_swr) == 30);
static_assert(offsetof(ps_cache_control_input_t, relay_origin_no_cache) == 31);
static_assert(offsetof(ps_cache_control_input_t, forward_origin_restrictions) ==
              32);

static_assert(sizeof(ps_cache_control_t) == 24);
static_assert(offsetof(ps_cache_control_t, struct_size) == 0);
static_assert(offsetof(ps_cache_control_t, max_age) == 8);
static_assert(offsetof(ps_cache_control_t, s_maxage) == 12);
static_assert(offsetof(ps_cache_control_t, cc_flags) == 16);

// ---------------------------------------------------------------------------
// The version triple itself is part of the contract: the gate reads it from
// the header, and the library reports it at runtime. They must agree, or a
// consumer's runtime feature check answers a different question than its
// compile-time one.
// ---------------------------------------------------------------------------
TEST(AbiLayout, ReportedVersionMatchesTheHeader) {
  EXPECT_EQ(ps_version_major(), PS_API_VERSION_MAJOR);
  EXPECT_EQ(ps_version_minor(), PS_API_VERSION_MINOR);
  EXPECT_EQ(ps_version_patch(), PS_API_VERSION_PATCH);
}

// The 1.2 surface is additive: 1.x consumers keep resolving what they had.
TEST(AbiLayout, VersionIsAtLeastTheSurfaceThisTestPins) {
  EXPECT_EQ(PS_API_VERSION_MAJOR, 1);
  EXPECT_GE(PS_API_VERSION_MINOR, 2);
}

}  // namespace
