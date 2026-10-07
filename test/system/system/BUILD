# Copyright 2024 Google LLC
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

licenses(["notice"])  # Apache 2

load("//bazel:pagespeed_python_test.bzl", "pagespeed_py_test", "pagespeed_py_test_suite")

# =============================================================================
# Python System Tests - System
# =============================================================================
#
# These tests are ported from pagespeed/system/system_tests/*.sh
# They typically require more complex server configurations.
#
# Run all tests:
#   bazel test //test/system/system:all --test_output=streamed \
#       --test_env=PAGESPEED_HOST=localhost --test_env=PAGESPEED_PORT=8080 \
#       --test_env=PAGESPEED_SECONDARY_HOST=localhost --test_env=PAGESPEED_SECONDARY_PORT=8081

pagespeed_py_test_suite(
    name = "all",
    srcs = [
        "test_add_resource_headers.py",
        "test_admin_gate.py",
        "test_admin_pages.py",
        "test_apache_static_asset_route.py",
        "test_aris.py",
        "test_authorization_basic.py",
        "test_beacons.py",
        "test_blocking_rewrite.py",
        "test_broken_fetch_ipro_record.py",
        "test_cache_compression_pre_gzipping.py",
        "test_cache_flush.py",
        "test_cache_purge.py",
        "test_compressed_cache.py",
        "test_content_encoding_leak.py",
        "test_critical_css_beacon.py",
        "test_cross_site_fetch.py",
        "test_css_combining_authorization.py",
        "test_custom_fetch_headers.py",
        "test_domain_rewrite.py",
        "test_downstream_cache.py",
        "test_embed_config.py",
        "test_encoded_absolute_urls.py",
        "test_experiments.py",
        "test_flush_handling.py",
        "test_forbid_all_disabled.py",
        "test_forbid_filters.py",
        "test_gce_public_cache.py",
        "test_handler_access.py",
        "test_handler_quoting.py",
        "test_headers.py",
        "test_htaccess_override.py",
        "test_image_quality_and_response.py",
        "test_image_rewrite_locking.py",
        "test_inline_unauth.py",
        "test_instant_ipro.py",
        "test_ipro.py",
        "test_ipro_for_browser.py",
        "test_ipro_proxy.py",
        "test_ipro_vary_cookie.py",
        "test_json_content_type.py",
        "test_lane_fixtures.py",
        "test_load_from_file.py",
        "test_map_proxy_domain.py",
        "test_map_proxy_domain_for_cdn.py",
        "test_mapped_domains.py",
        "test_max_cacheable.py",
        "test_max_html_parse_bytes.py",
        "test_mod_rewrite.py",
        "test_modify_caching_headers.py",
        "test_more_custom_headers.py",
        "test_no_critical_unauthorized.py",
        "test_no_transform.py",
        "test_optimize_for_bandwidth.py",
        "test_pagespeed_modes.py",
        "test_pass_through_headers.py",
        "test_preserve_urls.py",
        "test_prioritize_critical_images.py",
        "test_proxying.py",
        "test_query_params_core_filters.py",
        "test_remote_config.py",
        "test_request_option_override.py",
        "test_resource_ext_corruption.py",
        "test_respect_custom_options.py",
        "test_response_headers.py",
        "test_rewrite_deadline_ipro.py",
        "test_sane_connection_header.py",
        "test_server_side_includes.py",
        "test_shard_domain.py",
        "test_shm_cache.py",
        "test_show_cache.py",
        "test_smaxage.py",
        "test_source_maps.py",
        "test_statistics_local_only.py",
        "test_statistics_logging.py",
        "test_url_valued_attributes.py",
        "test_vary.py",
        "test_vhost_inheritance.py",
        "test_x_forwarded_proto.py",
        "test_x_sendfile.py",
        "test_zz_error_log_leak_sweep.py",
    ],
    deps = ["//test/system:conftest"],
)
